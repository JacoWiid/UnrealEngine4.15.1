// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	ScreenSpaceReflections.cpp: Post processing Screen Space Reflections implementation.
=============================================================================*/

#include "RendererPrivate.h"
#include "ScenePrivate.h"
#include "SceneFilterRendering.h"
#include "PostProcessing.h"
#include "ScreenSpaceReflections.h"
#include "PostProcessTemporalAA.h"
#include "PostProcessAmbientOcclusion.h"
#include "SceneUtils.h"

static TAutoConsoleVariable<int32> CVarSSRQuality(
	TEXT("r.SSR.Quality"),
	3,
	TEXT("Whether to use screen space reflections and at what quality setting.\n")
	TEXT("(limits the setting in the post process settings which has a different scale)\n")
	TEXT("(costs performance, adds more visual realism but the technique has limits)\n")
	TEXT(" 0: off (default)\n")
	TEXT(" 1: low (no glossy)\n")
	TEXT(" 2: medium (no glossy)\n")
	TEXT(" 3: high (glossy/using roughness, few samples)\n")
	TEXT(" 4: very high (likely too slow for real-time)"),
	ECVF_Scalability | ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarSSRTemporal(
	TEXT("r.SSR.Temporal"),
	0,
	TEXT("Defines if we use the temporal smoothing for the screen space reflection\n")
	TEXT(" 0 is off (for debugging), 1 is on (default)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarSSRStencil(
	TEXT("r.SSR.Stencil"),
	0,
	TEXT("Defines if we use the stencil prepass for the screen space reflection\n")
	TEXT(" 0 is off (default), 1 is on"),
	ECVF_RenderThreadSafe);

bool DoScreenSpaceReflections(const FViewInfo& View)
{
	if(!View.Family->EngineShowFlags.ScreenSpaceReflections)
	{
		return false;
	}

	if(!View.State)
	{
		// not view state (e.g. thumbnail rendering?), no HZB (no screen space reflections or occlusion culling)
		return false;
	}

	int SSRQuality = CVarSSRQuality.GetValueOnRenderThread();

	if(SSRQuality <= 0)
	{
		return false;
	}

	if(View.FinalPostProcessSettings.ScreenSpaceReflectionIntensity < 1.0f)
	{
		return false;
	}

	return true;
}

static float ComputeRoughnessMaskScale(const FRenderingCompositePassContext& Context, uint32 SSRQuality)
{
	float MaxRoughness = FMath::Clamp(Context.View.FinalPostProcessSettings.ScreenSpaceReflectionMaxRoughness, 0.01f, 1.0f);

	// f(x) = x * Scale + Bias
	// f(MaxRoughness) = 0
	// f(MaxRoughness/2) = 1

	float RoughnessMaskScale = -2.0f / MaxRoughness;
	return RoughnessMaskScale * (SSRQuality < 3 ? 2.0f : 1.0f);
}

/**
 * Encapsulates the post processing screen space reflections pixel shader stencil pass.
 */
class FPostProcessScreenSpaceReflectionsStencilPS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FPostProcessScreenSpaceReflectionsStencilPS, Global);

	static bool ShouldCache(EShaderPlatform Platform)
	{
		return IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM4);
	}

	static void ModifyCompilationEnvironment(EShaderPlatform Platform, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Platform, OutEnvironment);
		OutEnvironment.SetDefine( TEXT("PREV_FRAME_COLOR"), uint32(0) );
		OutEnvironment.SetDefine( TEXT("SSR_QUALITY"), uint32(0) );
	}

	/** Default constructor. */
	FPostProcessScreenSpaceReflectionsStencilPS() {}

public:
	FPostProcessPassParameters PostprocessParameter;
	FDeferredPixelShaderParameters DeferredParameters;
	FShaderParameter SSRParams;

	/** Initialization constructor. */
	FPostProcessScreenSpaceReflectionsStencilPS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{
		PostprocessParameter.Bind(Initializer.ParameterMap);
		DeferredParameters.Bind(Initializer.ParameterMap);
		SSRParams.Bind(Initializer.ParameterMap, TEXT("SSRParams"));
	}

	void SetParameters(const FRenderingCompositePassContext& Context, uint32 SSRQuality, bool EnableDiscard)
	{
		const FFinalPostProcessSettings& Settings = Context.View.FinalPostProcessSettings;
		const FPixelShaderRHIParamRef ShaderRHI = GetPixelShader();

		FGlobalShader::SetParameters(Context.RHICmdList, ShaderRHI, Context.View);

		PostprocessParameter.SetPS(ShaderRHI, Context, TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI());
		DeferredParameters.Set(Context.RHICmdList, ShaderRHI, Context.View);

		{
			float RoughnessMaskScale = ComputeRoughnessMaskScale(Context, SSRQuality);

			FLinearColor Value(
				FMath::Clamp(Context.View.FinalPostProcessSettings.ScreenSpaceReflectionIntensity * 0.01f, 0.0f, 1.0f), 
				RoughnessMaskScale,
				float(EnableDiscard), //TODO
				0);

			SetShaderValue(Context.RHICmdList, ShaderRHI, SSRParams, Value);
		}
	}

	// FShader interface.
	virtual bool Serialize(FArchive& Ar) override
	{
		bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
		Ar << PostprocessParameter << DeferredParameters << SSRParams;
		return bShaderHasOutdatedParameters;
	}
};

IMPLEMENT_SHADER_TYPE(,FPostProcessScreenSpaceReflectionsStencilPS,TEXT("ScreenSpaceReflections"),TEXT("ScreenSpaceReflectionsStencilPS"),SF_Pixel);

/**
 * Encapsulates the post processing screen space reflections pixel shader.
 * @param SSRQuality 0:Visualize Mask
 */
template<uint32 PrevFrame, uint32 SSRQuality >
class FPostProcessScreenSpaceReflectionsPS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FPostProcessScreenSpaceReflectionsPS, Global);

	static bool ShouldCache(EShaderPlatform Platform)
	{
		return IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM4);
	}

	static void ModifyCompilationEnvironment(EShaderPlatform Platform, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Platform, OutEnvironment);
		OutEnvironment.SetDefine( TEXT("PREV_FRAME_COLOR"), PrevFrame );
		OutEnvironment.SetDefine( TEXT("SSR_QUALITY"), SSRQuality );
	}

	/** Default constructor. */
	FPostProcessScreenSpaceReflectionsPS() {}

public:
	FPostProcessPassParameters PostprocessParameter;
	FDeferredPixelShaderParameters DeferredParameters;
	FShaderParameter SSRParams;
	FShaderParameter HZBUvFactorAndInvFactor;

	/** Initialization constructor. */
	FPostProcessScreenSpaceReflectionsPS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{
		PostprocessParameter.Bind(Initializer.ParameterMap);
		DeferredParameters.Bind(Initializer.ParameterMap);
		SSRParams.Bind(Initializer.ParameterMap, TEXT("SSRParams"));
		HZBUvFactorAndInvFactor.Bind(Initializer.ParameterMap, TEXT("HZBUvFactorAndInvFactor"));
	}

	void SetParameters(const FRenderingCompositePassContext& Context)
	{
		const FFinalPostProcessSettings& Settings = Context.View.FinalPostProcessSettings;
		const FPixelShaderRHIParamRef ShaderRHI = GetPixelShader();

		FGlobalShader::SetParameters(Context.RHICmdList, ShaderRHI, Context.View);

		PostprocessParameter.SetPS(ShaderRHI, Context, TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI());
		DeferredParameters.Set(Context.RHICmdList, ShaderRHI, Context.View);

		{
			float RoughnessMaskScale = ComputeRoughnessMaskScale(Context, SSRQuality);

			FLinearColor Value(
				FMath::Clamp(Context.View.FinalPostProcessSettings.ScreenSpaceReflectionIntensity * 0.01f, 0.0f, 1.0f), 
				RoughnessMaskScale,
				0, 
				0);

			SetShaderValue(Context.RHICmdList, ShaderRHI, SSRParams, Value);
		}

		{
			const FVector2D HZBUvFactor(
				float(Context.View.ViewRect.Width()) / float(2 * Context.View.HZBMipmap0Size.X),
				float(Context.View.ViewRect.Height()) / float(2 * Context.View.HZBMipmap0Size.Y)
				);
			const FVector4 HZBUvFactorAndInvFactorValue(
				HZBUvFactor.X,
				HZBUvFactor.Y,
				1.0f / HZBUvFactor.X,
				1.0f / HZBUvFactor.Y
				);
			
			SetShaderValue(Context.RHICmdList, ShaderRHI, HZBUvFactorAndInvFactor, HZBUvFactorAndInvFactorValue);
		}
	}

	// FShader interface.
	virtual bool Serialize(FArchive& Ar) override
	{
		bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
		Ar << PostprocessParameter << DeferredParameters << SSRParams << HZBUvFactorAndInvFactor;
		return bShaderHasOutdatedParameters;
	}
};

// Typedef is necessary because the C preprocessor thinks the comma in the template parameter list is a comma in the macro parameter list.
#define IMPLEMENT_REFLECTION_PIXELSHADER_TYPE(A, B) \
	typedef FPostProcessScreenSpaceReflectionsPS<A,B> FPostProcessScreenSpaceReflectionsPS##A##B; \
	IMPLEMENT_SHADER_TYPE(template<>,FPostProcessScreenSpaceReflectionsPS##A##B,TEXT("ScreenSpaceReflections"),TEXT("ScreenSpaceReflectionsPS"),SF_Pixel)

IMPLEMENT_REFLECTION_PIXELSHADER_TYPE(0,0);
IMPLEMENT_REFLECTION_PIXELSHADER_TYPE(0,1);
IMPLEMENT_REFLECTION_PIXELSHADER_TYPE(1,1);
IMPLEMENT_REFLECTION_PIXELSHADER_TYPE(0,2);
IMPLEMENT_REFLECTION_PIXELSHADER_TYPE(1,2);
IMPLEMENT_REFLECTION_PIXELSHADER_TYPE(0,3);
IMPLEMENT_REFLECTION_PIXELSHADER_TYPE(1,3);
IMPLEMENT_REFLECTION_PIXELSHADER_TYPE(0,4);
IMPLEMENT_REFLECTION_PIXELSHADER_TYPE(1,4);

// --------------------------------------------------------


// @param Quality usually in 0..100 range, default is 50
// @return see CVarSSRQuality, never 0
static int32 ComputeSSRQuality(float Quality)
{
	int32 Ret;

	if(Quality >= 60.0f)
	{
		Ret = (Quality >= 80.0f) ? 4 : 3;
	}
	else
	{
		Ret = (Quality >= 40.0f) ? 2 : 1;
	}

	int SSRQualityCVar = FMath::Max(0, CVarSSRQuality.GetValueOnRenderThread());

	return FMath::Min(Ret, SSRQualityCVar);
}

void FRCPassPostProcessScreenSpaceReflections::Process(FRenderingCompositePassContext& Context)
{
	auto& RHICmdList = Context.RHICmdList;
	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);

	const FSceneView& View = Context.View;
	const auto FeatureLevel = Context.GetFeatureLevel();

	int32 SSRQuality = ComputeSSRQuality(View.FinalPostProcessSettings.ScreenSpaceReflectionQuality);
	uint32 iPreFrame = bPrevFrame ? 1 : 0;

	SSRQuality = FMath::Clamp(SSRQuality, 1, 4);
	
	if (View.Family->EngineShowFlags.VisualizeSSR)
	{
		iPreFrame = 0;
		SSRQuality = 0;
	}
	
	// Write 1 to highest bit of stencil to areas that should compute SSR
	bool SSRStencilPrePass = CVarSSRStencil.GetValueOnRenderThread() != 0 && SSRQuality != 0;
	
	const FSceneRenderTargetItem& DestRenderTarget = PassOutputs[0].RequestSurface(Context);

	if (SSRStencilPrePass)
	{ // ScreenSpaceReflectionsStencil draw event
		SCOPED_DRAW_EVENT(RHICmdList, ScreenSpaceReflectionsStencil);

		TShaderMapRef< FPostProcessVS > VertexShader(Context.GetShaderMap());
		TShaderMapRef< FPostProcessScreenSpaceReflectionsStencilPS > PixelShader(Context.GetShaderMap());
		
		// bind the dest render target and the depth stencil render target
		SetRenderTarget(RHICmdList, DestRenderTarget.TargetableTexture, SceneContext.GetSceneDepthSurface(), ESimpleRenderTargetMode::EUninitializedColorAndDepth, FExclusiveDepthStencil::DepthRead_StencilWrite);
		Context.SetViewportAndCallRHI(View.ViewRect);

		// Clear stencil to 0
		RHICmdList.Clear(false, FLinearColor::White, false, (float)ERHIZBuffer::FarPlane, true, 0, View.ViewRect);
		
		// bind shader
		static FGlobalBoundShaderState BoundShaderState;
		SetGlobalBoundShaderState(Context.RHICmdList, FeatureLevel, BoundShaderState, GFilterVertexDeclaration.VertexDeclarationRHI, *VertexShader, *PixelShader);
		VertexShader->SetParameters(Context);
		PixelShader->SetParameters(Context, SSRQuality, true);
		
		// Clobers the stencil to pixel that should not compute SSR
		RHICmdList.SetDepthStencilState(TStaticDepthStencilState<false, CF_Always, true, CF_Always, SO_Replace, SO_Replace, SO_Replace>::GetRHI(), 0x80);

		// Set rasterizer state to solid
		RHICmdList.SetRasterizerState(TStaticRasterizerState<>::GetRHI());

		// disable blend mode
		RHICmdList.SetBlendState(TStaticBlendState<>::GetRHI());
	
		// Draw a quad mapping scene color to the view's render target to set stencil to set the stencil mask where it needs to be
		DrawRectangle( 
			Context.RHICmdList,
			0, 0,
			View.ViewRect.Width(), View.ViewRect.Height(),
			View.ViewRect.Min.X, View.ViewRect.Min.Y, 
			View.ViewRect.Width(), View.ViewRect.Height(),
			View.ViewRect.Size(),
			SceneContext.GetBufferSizeXY(),
			*VertexShader,
			EDRF_UseTriangleOptimization);
	} // ScreenSpaceReflectionsStencil draw event

	{ // ScreenSpaceReflections draw event
		SCOPED_DRAW_EVENT(Context.RHICmdList, ScreenSpaceReflections);

		if (SSRStencilPrePass)
		{
			// set up the stencil test to match 0, meaning FPostProcessScreenSpaceReflectionsStencilPS has been discarded
			RHICmdList.SetDepthStencilState(TStaticDepthStencilState<false, CF_Always, true, CF_Equal, SO_Keep, SO_Keep, SO_Keep>::GetRHI(), 0);
		}
		else
	{
			// bind only the dest render target
			SetRenderTarget(RHICmdList, DestRenderTarget.TargetableTexture, FTextureRHIRef());
			Context.SetViewportAndCallRHI(View.ViewRect);

			RHICmdList.SetDepthStencilState(TStaticDepthStencilState<false, CF_Always>::GetRHI());
	}

		// clear DestRenderTarget only outside of the view's rectangle
		RHICmdList.Clear(true, FLinearColor::Black, false, (float)ERHIZBuffer::FarPlane, false, 0, View.ViewRect);
		
		// set the state
		RHICmdList.SetBlendState(TStaticBlendState<>::GetRHI());
		RHICmdList.SetRasterizerState(TStaticRasterizerState<>::GetRHI());

	TShaderMapRef< FPostProcessVS > VertexShader(Context.GetShaderMap());

	#define CASE(A, B) \
		case (A + 2 * (B + 3 * 0 )): \
		{ \
			TShaderMapRef< FPostProcessScreenSpaceReflectionsPS<A, B> > PixelShader(Context.GetShaderMap()); \
			static FGlobalBoundShaderState BoundShaderState; \
				SetGlobalBoundShaderState(RHICmdList, FeatureLevel, BoundShaderState, GFilterVertexDeclaration.VertexDeclarationRHI, *VertexShader, *PixelShader); \
			VertexShader->SetParameters(Context); \
			PixelShader->SetParameters(Context); \
		}; \
		break

	switch (iPreFrame + 2 * (SSRQuality + 3 * 0))
	{
		CASE(0,0);
		CASE(0,1);	CASE(1,1);
		CASE(0,2);	CASE(1,2);
		CASE(0,3);	CASE(1,3);
		CASE(0,4);	CASE(1,4);
		default:
			check(!"Missing case in FRCPassPostProcessScreenSpaceReflections");
	}
	#undef CASE


	// Draw a quad mapping scene color to the view's render target
	DrawRectangle( 
			RHICmdList,
		0, 0,
		View.ViewRect.Width(), View.ViewRect.Height(),
		View.ViewRect.Min.X, View.ViewRect.Min.Y, 
		View.ViewRect.Width(), View.ViewRect.Height(),
		View.ViewRect.Size(),
		FSceneRenderTargets::Get(Context.RHICmdList).GetBufferSizeXY(),
		*VertexShader,
		EDRF_UseTriangleOptimization);

		RHICmdList.CopyToResolveTarget(DestRenderTarget.TargetableTexture, DestRenderTarget.ShaderResourceTexture, false, FResolveParams());
	} // ScreenSpaceReflections
}

FPooledRenderTargetDesc FRCPassPostProcessScreenSpaceReflections::ComputeOutputDesc(EPassOutputId InPassOutputId) const
{
	FPooledRenderTargetDesc Ret(FPooledRenderTargetDesc::Create2DDesc(FSceneRenderTargets::Get_FrameConstantsOnly().GetBufferSizeXY(), PF_FloatRGBA, TexCreate_None, TexCreate_RenderTargetable, false));

	Ret.DebugName = TEXT("ScreenSpaceReflections");

	return Ret;
}

void ScreenSpaceReflections(FRHICommandListImmediate& RHICmdList, FViewInfo& View, TRefCountPtr<IPooledRenderTarget>& SSROutput)
{
	FRenderingCompositePassContext CompositeContext(RHICmdList, View);	
	FPostprocessContext Context( CompositeContext.Graph, View );

	FSceneViewState* ViewState = (FSceneViewState*)Context.View.State;

	FRenderingCompositePass* SceneColorInput = Context.Graph.RegisterPass( new FRCPassPostProcessInput( FSceneRenderTargets::Get(RHICmdList).GetSceneColor() ) );
	FRenderingCompositePass* HZBInput = Context.Graph.RegisterPass( new FRCPassPostProcessInput( View.HZB ) );

	bool bPrevFrame = 0;
	if( ViewState && ViewState->TemporalAAHistoryRT && !Context.View.bCameraCut )
	{
		SceneColorInput = Context.Graph.RegisterPass( new FRCPassPostProcessInput( ViewState->TemporalAAHistoryRT ) );
		bPrevFrame = 1;
	}

	{
		FRenderingCompositePass* TracePass = Context.Graph.RegisterPass( new FRCPassPostProcessScreenSpaceReflections( bPrevFrame ) );
		TracePass->SetInput( ePId_Input0, SceneColorInput );
		TracePass->SetInput( ePId_Input1, HZBInput );

		Context.FinalOutput = FRenderingCompositeOutputRef( TracePass );
	}

	const bool bTemporalFilter = View.FinalPostProcessSettings.AntiAliasingMethod != AAM_TemporalAA || CVarSSRTemporal.GetValueOnRenderThread() != 0;

	if( ViewState && bTemporalFilter )
	{
		{
			FRenderingCompositeOutputRef HistoryInput;
			if( ViewState && ViewState->SSRHistoryRT && !Context.View.bCameraCut )
			{
				HistoryInput = Context.Graph.RegisterPass( new FRCPassPostProcessInput( ViewState->SSRHistoryRT ) );
			}
			else
			{
				// No history, use black
				HistoryInput = Context.Graph.RegisterPass(new FRCPassPostProcessInput(GSystemTextures.BlackDummy));
			}

			FRenderingCompositePass* TemporalAAPass = Context.Graph.RegisterPass( new FRCPassPostProcessSSRTemporalAA );
			TemporalAAPass->SetInput( ePId_Input0, Context.FinalOutput );
			TemporalAAPass->SetInput( ePId_Input1, HistoryInput );
			//TemporalAAPass->SetInput( ePId_Input2, VelocityInput );

			Context.FinalOutput = FRenderingCompositeOutputRef( TemporalAAPass );
		}

		if( ViewState )
		{
			FRenderingCompositePass* HistoryOutput = Context.Graph.RegisterPass( new FRCPassPostProcessOutput( &ViewState->SSRHistoryRT ) );
			HistoryOutput->SetInput( ePId_Input0, Context.FinalOutput );

			Context.FinalOutput = FRenderingCompositeOutputRef( HistoryOutput );
		}
	}

	{
		FRenderingCompositePass* ReflectionOutput = Context.Graph.RegisterPass( new FRCPassPostProcessOutput( &SSROutput ) );
		ReflectionOutput->SetInput( ePId_Input0, Context.FinalOutput );

		Context.FinalOutput = FRenderingCompositeOutputRef( ReflectionOutput );
	}

	CompositeContext.Process(Context.FinalOutput.GetPass(), TEXT("ReflectionEnvironments"));
}
