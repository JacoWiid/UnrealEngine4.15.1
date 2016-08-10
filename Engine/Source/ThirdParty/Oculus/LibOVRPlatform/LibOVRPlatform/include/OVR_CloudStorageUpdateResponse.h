// This file was @generated with LibOVRPlatform/codegen/main. Do not modify it!

#ifndef OVR_CLOUDSTORAGEUPDATERESPONSE_H
#define OVR_CLOUDSTORAGEUPDATERESPONSE_H

#include "OVR_Platform_Defs.h"
#include "OVR_CloudStorage.h"
#include "OVR_CloudStorageUpdateStatus.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct ovrCloudStorageUpdateResponse *ovrCloudStorageUpdateResponseHandle;

OVRP_PUBLIC_FUNCTION(const char *)                ovr_CloudStorageUpdateResponse_GetBucket(const ovrCloudStorageUpdateResponseHandle obj);
OVRP_PUBLIC_FUNCTION(ovrCloudStorageSaveHandle)   ovr_CloudStorageUpdateResponse_GetHandle(const ovrCloudStorageUpdateResponseHandle obj);
OVRP_PUBLIC_FUNCTION(const char *)                ovr_CloudStorageUpdateResponse_GetKey(const ovrCloudStorageUpdateResponseHandle obj);
OVRP_PUBLIC_FUNCTION(ovrCloudStorageUpdateStatus) ovr_CloudStorageUpdateResponse_GetStatus(const ovrCloudStorageUpdateResponseHandle obj);

#endif