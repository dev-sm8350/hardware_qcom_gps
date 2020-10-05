/* Copyright (c) 2017-2020 The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation, nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
#define LOG_NDEBUG 0
#define LOG_TAG "LocSvc_GnssAdapter"

#include <inttypes.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#include <cutils/properties.h>
#include <math.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <GnssAdapter.h>
#include <string>
#include <sstream>
#include <loc_log.h>
#include <loc_nmea.h>
#include <Agps.h>
#include <SystemStatus.h>
#include <vector>
#include <loc_misc_utils.h>

#define RAD2DEG    (180.0 / M_PI)
#define DEG2RAD    (M_PI / 180.0)
#define PROCESS_NAME_ENGINE_SERVICE "engine-service"
#define BILLION_NSEC (1000000000ULL)
#define NMEA_MIN_THRESHOLD_MSEC (99)
#define NMEA_MAX_THRESHOLD_MSEC (975)
using namespace loc_core;

static int loadEngHubForExternalEngine = 0;
static loc_param_s_type izatConfParamTable[] = {
    {"LOAD_ENGHUB_FOR_EXTERNAL_ENGINE", &loadEngHubForExternalEngine, nullptr,'n'}
};

/* Method to fetch status cb from loc_net_iface library */
typedef AgpsCbInfo& (*LocAgpsGetAgpsCbInfo)(LocAgpsOpenResultCb openResultCb,
        LocAgpsCloseResultCb closeResultCb, void* userDataPtr);

static void agpsOpenResultCb (bool isSuccess, AGpsExtType agpsType, const char* apn,
        AGpsBearerType bearerType, void* userDataPtr);
static void agpsCloseResultCb (bool isSuccess, AGpsExtType agpsType, void* userDataPtr);

typedef const CdfwInterface* (*getCdfwInterface)();

GnssAdapter::GnssAdapter() :
    LocAdapterBase(0,
                   LocDualContext::getLocFgContext(NULL,
                                                   NULL,
                                                   LocDualContext::mLocationHalName,
                                                   false),
                   true, nullptr, true),
    mEngHubProxy(new EngineHubProxyBase()),
    mQDgnssListenerHDL(nullptr),
    mCdfwInterface(nullptr),
    mDGnssNeedReport(false),
    mDGnssDataUsage(false),
    mLocPositionMode(),
    mNHzNeeded(false),
    mSPEAlreadyRunningAtHighestInterval(false),
    mGnssSvIdUsedInPosition(),
    mGnssSvIdUsedInPosAvail(false),
    mControlCallbacks(),
    mPowerVoteId(0),
    mNmeaMask(0),
    mGnssSvIdConfig(),
    mGnssSeconaryBandConfig(),
    mGnssSvTypeConfig(),
    mGnssSvTypeConfigCb(nullptr),
    mLocConfigInfo{},
    mNiData(),
    mAgpsManager(),
    mAgpsCbInfo(),
    mOdcpiRequestCb(nullptr),
    mOdcpiRequestActive(false),
    mOdcpiTimer(this),
    mOdcpiRequest(),
    mSystemStatus(SystemStatus::getInstance(mMsgTask)),
    mServerUrl(":"),
    mXtraObserver(mSystemStatus->getOsObserver(), mMsgTask),
    mBlockCPIInfo{},
    mLocSystemInfo{},
    mGnssMbSvIdUsedInPosition{},
    mGnssMbSvIdUsedInPosAvail(false),
    mPowerState(POWER_STATE_UNKNOWN)
{
    LOC_LOGD("%s]: Constructor %p", __func__, this);
    mLocPositionMode.mode = LOC_POSITION_MODE_INVALID;

    pthread_condattr_t condAttr;
    pthread_condattr_init(&condAttr);
    pthread_condattr_setclock(&condAttr, CLOCK_MONOTONIC);
    pthread_cond_init(&mNiData.session.tCond, &condAttr);
    pthread_cond_init(&mNiData.sessionEs.tCond, &condAttr);
    pthread_condattr_destroy(&condAttr);

    /* Set ATL open/close callbacks */
    AgpsAtlOpenStatusCb atlOpenStatusCb =
            [this](int handle, int isSuccess, char* apn, uint32_t apnLen,
                    AGpsBearerType bearerType, AGpsExtType agpsType, LocApnTypeMask mask) {

                mLocApi->atlOpenStatus(
                        handle, isSuccess, apn, apnLen, bearerType, agpsType, mask);
            };
    AgpsAtlCloseStatusCb atlCloseStatusCb =
            [this](int handle, int isSuccess) {

                mLocApi->atlCloseStatus(handle, isSuccess);
            };
    mAgpsManager.registerATLCallbacks(atlOpenStatusCb, atlCloseStatusCb);

    readConfigCommand();
    initDefaultAgpsCommand();
    initEngHubProxyCommand();
    initDGnssUsableReporter();

    // at last step, let us inform adapater base that we are done
    // with initialization, e.g.: ready to process handleEngineUpEvent
    doneInit();
}

void
GnssAdapter::setControlCallbacksCommand(LocationControlCallbacks& controlCallbacks)
{
    struct MsgSetControlCallbacks : public LocMsg {
        GnssAdapter& mAdapter;
        const LocationControlCallbacks mControlCallbacks;
        inline MsgSetControlCallbacks(GnssAdapter& adapter,
                                      LocationControlCallbacks& controlCallbacks) :
            LocMsg(),
            mAdapter(adapter),
            mControlCallbacks(controlCallbacks) {}
        inline virtual void proc() const {
            mAdapter.setControlCallbacks(mControlCallbacks);
        }
    };

    sendMsg(new MsgSetControlCallbacks(*this, controlCallbacks));
}

void
GnssAdapter::convertOptions(LocPosMode& out, const TrackingOptions& trackingOptions)
{
    switch (trackingOptions.mode) {
    case GNSS_SUPL_MODE_MSB:
        out.mode = LOC_POSITION_MODE_MS_BASED;
        break;
    case GNSS_SUPL_MODE_MSA:
        out.mode = LOC_POSITION_MODE_MS_ASSISTED;
        break;
    default:
        out.mode = LOC_POSITION_MODE_STANDALONE;
        break;
    }
    out.share_position = true;
    out.min_interval = trackingOptions.minInterval;
    out.powerMode = trackingOptions.powerMode;
    out.timeBetweenMeasurements = trackingOptions.tbm;
}

bool
GnssAdapter::checkAndSetSPEToRunforNHz(LocPosMode & out) {
    // first check if NHz meas is needed at all, if not, just return false
    // if a NHz capable engine is subscribed for NHz measurement or NHz positions,
    // always run the SPE only session at 100ms TBF.
    // If SPE session is already set to highest interval, no need to start it again.

    bool isSPERunningAtHighestInterval = false;

    if (!mNHzNeeded) {
        LOC_LOGd("No nHz session needed.");
    } else if (mSPEAlreadyRunningAtHighestInterval) {
        LOC_LOGd("SPE is already running at highest interval.");
        isSPERunningAtHighestInterval = true;
    } else if (out.min_interval > MIN_TRACKING_INTERVAL) {
        out.min_interval = MIN_TRACKING_INTERVAL;
        LOC_LOGd("nHz session is needed, starting SPE only session at 100ms TBF.");
        mSPEAlreadyRunningAtHighestInterval = true;
    }

    return isSPERunningAtHighestInterval;
}


void
GnssAdapter::convertLocation(Location& out, const UlpLocation& ulpLocation,
                             const GpsLocationExtended& locationExtended,
                             const LocPosTechMask techMask)
{
    memset(&out, 0, sizeof(Location));
    out.size = sizeof(Location);
    if (LOC_GPS_LOCATION_HAS_LAT_LONG & ulpLocation.gpsLocation.flags) {
        out.flags |= LOCATION_HAS_LAT_LONG_BIT;
        out.latitude = ulpLocation.gpsLocation.latitude;
        out.longitude = ulpLocation.gpsLocation.longitude;
    }
    if (LOC_GPS_LOCATION_HAS_ALTITUDE & ulpLocation.gpsLocation.flags) {
        out.flags |= LOCATION_HAS_ALTITUDE_BIT;
        out.altitude = ulpLocation.gpsLocation.altitude;
    }
    if (LOC_GPS_LOCATION_HAS_SPEED & ulpLocation.gpsLocation.flags) {
        out.flags |= LOCATION_HAS_SPEED_BIT;
        out.speed = ulpLocation.gpsLocation.speed;
    }
    if (LOC_GPS_LOCATION_HAS_BEARING & ulpLocation.gpsLocation.flags) {
        out.flags |= LOCATION_HAS_BEARING_BIT;
        out.bearing = ulpLocation.gpsLocation.bearing;
    }
    if (LOC_GPS_LOCATION_HAS_ACCURACY & ulpLocation.gpsLocation.flags) {
        out.flags |= LOCATION_HAS_ACCURACY_BIT;
        out.accuracy = ulpLocation.gpsLocation.accuracy;
    }
    if (GPS_LOCATION_EXTENDED_HAS_VERT_UNC & locationExtended.flags) {
        out.flags |= LOCATION_HAS_VERTICAL_ACCURACY_BIT;
        out.verticalAccuracy = locationExtended.vert_unc;
    }
    if (GPS_LOCATION_EXTENDED_HAS_SPEED_UNC & locationExtended.flags) {
        out.flags |= LOCATION_HAS_SPEED_ACCURACY_BIT;
        out.speedAccuracy = locationExtended.speed_unc;
    }
    if (GPS_LOCATION_EXTENDED_HAS_BEARING_UNC & locationExtended.flags) {
        out.flags |= LOCATION_HAS_BEARING_ACCURACY_BIT;
        out.bearingAccuracy = locationExtended.bearing_unc;
    }
    out.timestamp = ulpLocation.gpsLocation.timestamp;
    if (LOC_POS_TECH_MASK_SATELLITE & techMask) {
        out.techMask |= LOCATION_TECHNOLOGY_GNSS_BIT;
    }
    if (LOC_POS_TECH_MASK_CELLID & techMask) {
        out.techMask |= LOCATION_TECHNOLOGY_CELL_BIT;
    }
    if (LOC_POS_TECH_MASK_WIFI & techMask) {
        out.techMask |= LOCATION_TECHNOLOGY_WIFI_BIT;
    }
    if (LOC_POS_TECH_MASK_SENSORS & techMask) {
        out.techMask |= LOCATION_TECHNOLOGY_SENSORS_BIT;
    }
    if (LOC_POS_TECH_MASK_REFERENCE_LOCATION & techMask) {
        out.techMask |= LOCATION_TECHNOLOGY_REFERENCE_LOCATION_BIT;
    }
    if (LOC_POS_TECH_MASK_INJECTED_COARSE_POSITION & techMask) {
        out.techMask |= LOCATION_TECHNOLOGY_INJECTED_COARSE_POSITION_BIT;
    }
    if (LOC_POS_TECH_MASK_AFLT & techMask) {
        out.techMask |= LOCATION_TECHNOLOGY_AFLT_BIT;
    }
    if (LOC_POS_TECH_MASK_HYBRID & techMask) {
        out.techMask |= LOCATION_TECHNOLOGY_HYBRID_BIT;
    }
    if (LOC_POS_TECH_MASK_PPE & techMask) {
        out.techMask |= LOCATION_TECHNOLOGY_PPE_BIT;
    }
    if (LOC_POS_TECH_MASK_VEH & techMask) {
        out.techMask |= LOCATION_TECHNOLOGY_VEH_BIT;
    }
    if (LOC_POS_TECH_MASK_VIS & techMask) {
        out.techMask |= LOCATION_TECHNOLOGY_VIS_BIT;
    }

    if (LOC_GPS_LOCATION_HAS_SPOOF_MASK & ulpLocation.gpsLocation.flags) {
        out.flags |= LOCATION_HAS_SPOOF_MASK;
        out.spoofMask = ulpLocation.gpsLocation.spoof_mask;
    }
}

/* This is utility routine that computes number of SV used
   in the fix from the svUsedIdsMask.
 */
#define MAX_SV_CNT_SUPPORTED_IN_ONE_CONSTELLATION 64
uint16_t GnssAdapter::getNumSvUsed(uint64_t svUsedIdsMask,
                                   int totalSvCntInThisConstellation)
{
    if (totalSvCntInThisConstellation > MAX_SV_CNT_SUPPORTED_IN_ONE_CONSTELLATION) {
        LOC_LOGe ("error: total SV count in this constellation %d exceeded limit of %d",
                  totalSvCntInThisConstellation, MAX_SV_CNT_SUPPORTED_IN_ONE_CONSTELLATION);
        return 0;
    }

    uint16_t numSvUsed = 0;
    uint64_t mask = 0x1;
    for (int i = 0; i < totalSvCntInThisConstellation; i++) {
        if (svUsedIdsMask & mask) {
            numSvUsed++;
        }
        mask <<= 1;
    }

    return numSvUsed;
}

void
GnssAdapter::convertLocationInfo(GnssLocationInfoNotification& out,
                                 const GpsLocationExtended& locationExtended)
{
    out.size = sizeof(GnssLocationInfoNotification);
    if (GPS_LOCATION_EXTENDED_HAS_ALTITUDE_MEAN_SEA_LEVEL & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_ALTITUDE_MEAN_SEA_LEVEL_BIT;
        out.altitudeMeanSeaLevel = locationExtended.altitudeMeanSeaLevel;
    }

    if (GPS_LOCATION_EXTENDED_HAS_EXT_DOP & locationExtended.flags) {
        out.flags |= (GNSS_LOCATION_INFO_DOP_BIT|GNSS_LOCATION_INFO_EXT_DOP_BIT);
        out.pdop = locationExtended.extDOP.PDOP;
        out.hdop = locationExtended.extDOP.HDOP;
        out.vdop = locationExtended.extDOP.VDOP;
        out.gdop = locationExtended.extDOP.GDOP;
        out.tdop = locationExtended.extDOP.TDOP;
    } else if (GPS_LOCATION_EXTENDED_HAS_DOP & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_DOP_BIT;
        out.pdop = locationExtended.pdop;
        out.hdop = locationExtended.hdop;
        out.vdop = locationExtended.vdop;
    }

    if (GPS_LOCATION_EXTENDED_HAS_MAG_DEV & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_MAGNETIC_DEVIATION_BIT;
        out.magneticDeviation = locationExtended.magneticDeviation;
    }
    if (GPS_LOCATION_EXTENDED_HAS_HOR_RELIABILITY & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_HOR_RELIABILITY_BIT;
        switch (locationExtended.horizontal_reliability) {
            case LOC_RELIABILITY_VERY_LOW:
                out.horReliability = LOCATION_RELIABILITY_VERY_LOW;
                break;
            case LOC_RELIABILITY_LOW:
                out.horReliability = LOCATION_RELIABILITY_LOW;
                break;
            case LOC_RELIABILITY_MEDIUM:
                out.horReliability = LOCATION_RELIABILITY_MEDIUM;
                break;
            case LOC_RELIABILITY_HIGH:
                out.horReliability = LOCATION_RELIABILITY_HIGH;
                break;
            default:
                out.horReliability = LOCATION_RELIABILITY_NOT_SET;
                break;
        }
    }
    if (GPS_LOCATION_EXTENDED_HAS_VERT_RELIABILITY & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_VER_RELIABILITY_BIT;
        switch (locationExtended.vertical_reliability) {
            case LOC_RELIABILITY_VERY_LOW:
                out.verReliability = LOCATION_RELIABILITY_VERY_LOW;
                break;
            case LOC_RELIABILITY_LOW:
                out.verReliability = LOCATION_RELIABILITY_LOW;
                break;
            case LOC_RELIABILITY_MEDIUM:
                out.verReliability = LOCATION_RELIABILITY_MEDIUM;
                break;
            case LOC_RELIABILITY_HIGH:
                out.verReliability = LOCATION_RELIABILITY_HIGH;
                break;
            default:
                out.verReliability = LOCATION_RELIABILITY_NOT_SET;
                break;
        }
    }
    if (GPS_LOCATION_EXTENDED_HAS_HOR_ELIP_UNC_MAJOR & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_HOR_ACCURACY_ELIP_SEMI_MAJOR_BIT;
        out.horUncEllipseSemiMajor = locationExtended.horUncEllipseSemiMajor;
    }
    if (GPS_LOCATION_EXTENDED_HAS_HOR_ELIP_UNC_MINOR & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_HOR_ACCURACY_ELIP_SEMI_MINOR_BIT;
        out.horUncEllipseSemiMinor = locationExtended.horUncEllipseSemiMinor;
    }
    if (GPS_LOCATION_EXTENDED_HAS_HOR_ELIP_UNC_AZIMUTH & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_HOR_ACCURACY_ELIP_AZIMUTH_BIT;
        out.horUncEllipseOrientAzimuth = locationExtended.horUncEllipseOrientAzimuth;
    }
    if (GPS_LOCATION_EXTENDED_HAS_NORTH_STD_DEV & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_NORTH_STD_DEV_BIT;
        out.northStdDeviation = locationExtended.northStdDeviation;
    }
    if (GPS_LOCATION_EXTENDED_HAS_EAST_STD_DEV & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_EAST_STD_DEV_BIT;
        out.eastStdDeviation = locationExtended.eastStdDeviation;
    }
    if (GPS_LOCATION_EXTENDED_HAS_NORTH_VEL & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_NORTH_VEL_BIT;
        out.northVelocity = locationExtended.northVelocity;
    }
    if (GPS_LOCATION_EXTENDED_HAS_NORTH_VEL_UNC & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_NORTH_VEL_UNC_BIT;
        out.northVelocityStdDeviation = locationExtended.northVelocityStdDeviation;
    }
    if (GPS_LOCATION_EXTENDED_HAS_EAST_VEL & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_EAST_VEL_BIT;
        out.eastVelocity = locationExtended.eastVelocity;
    }
    if (GPS_LOCATION_EXTENDED_HAS_EAST_VEL_UNC & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_EAST_VEL_UNC_BIT;
        out.eastVelocityStdDeviation = locationExtended.eastVelocityStdDeviation;
    }
    if (GPS_LOCATION_EXTENDED_HAS_UP_VEL & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_UP_VEL_BIT;
        out.upVelocity = locationExtended.upVelocity;
    }
    if (GPS_LOCATION_EXTENDED_HAS_UP_VEL_UNC & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_UP_VEL_UNC_BIT;
        out.upVelocityStdDeviation = locationExtended.upVelocityStdDeviation;
    }
    if (GPS_LOCATION_EXTENDED_HAS_GNSS_SV_USED_DATA & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_GNSS_SV_USED_DATA_BIT;
        out.svUsedInPosition.gpsSvUsedIdsMask =
                locationExtended.gnss_sv_used_ids.gps_sv_used_ids_mask;
        out.svUsedInPosition.gloSvUsedIdsMask =
                locationExtended.gnss_sv_used_ids.glo_sv_used_ids_mask;
        out.svUsedInPosition.galSvUsedIdsMask =
                locationExtended.gnss_sv_used_ids.gal_sv_used_ids_mask;
        out.svUsedInPosition.bdsSvUsedIdsMask =
                locationExtended.gnss_sv_used_ids.bds_sv_used_ids_mask;
        out.svUsedInPosition.qzssSvUsedIdsMask =
                locationExtended.gnss_sv_used_ids.qzss_sv_used_ids_mask;
        out.svUsedInPosition.navicSvUsedIdsMask =
                locationExtended.gnss_sv_used_ids.navic_sv_used_ids_mask;

        out.flags |= GNSS_LOCATION_INFO_NUM_SV_USED_IN_POSITION_BIT;
        out.numSvUsedInPosition = getNumSvUsed(out.svUsedInPosition.gpsSvUsedIdsMask,
                                               GPS_SV_PRN_MAX - GPS_SV_PRN_MIN + 1);
        out.numSvUsedInPosition += getNumSvUsed(out.svUsedInPosition.gloSvUsedIdsMask,
                                                GLO_SV_PRN_MAX - GLO_SV_PRN_MIN + 1);
        out.numSvUsedInPosition += getNumSvUsed(out.svUsedInPosition.qzssSvUsedIdsMask,
                                                QZSS_SV_PRN_MAX - QZSS_SV_PRN_MIN + 1);
        out.numSvUsedInPosition += getNumSvUsed(out.svUsedInPosition.bdsSvUsedIdsMask,
                                                BDS_SV_PRN_MAX - BDS_SV_PRN_MIN + 1);
        out.numSvUsedInPosition += getNumSvUsed(out.svUsedInPosition.galSvUsedIdsMask,
                                                GAL_SV_PRN_MAX - GAL_SV_PRN_MIN + 1);
        out.numSvUsedInPosition += getNumSvUsed(out.svUsedInPosition.navicSvUsedIdsMask,
                                                NAVIC_SV_PRN_MAX - NAVIC_SV_PRN_MIN + 1);

        out.numOfMeasReceived = locationExtended.numOfMeasReceived;
        for (int idx =0; idx < locationExtended.numOfMeasReceived; idx++) {
            out.measUsageInfo[idx].gnssSignalType =
                    locationExtended.measUsageInfo[idx].gnssSignalType;
            out.measUsageInfo[idx].gnssSvId =
                    locationExtended.measUsageInfo[idx].gnssSvId;
            out.measUsageInfo[idx].gnssConstellation =
                    locationExtended.measUsageInfo[idx].gnssConstellation;
        }
    }
    if (GPS_LOCATION_EXTENDED_HAS_NAV_SOLUTION_MASK & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_NAV_SOLUTION_MASK_BIT;
        out.navSolutionMask = locationExtended.navSolutionMask;
    }
    if (GPS_LOCATION_EXTENDED_HAS_POS_DYNAMICS_DATA & locationExtended.flags) {
        out.flags |= GPS_LOCATION_EXTENDED_HAS_POS_DYNAMICS_DATA;
        if (locationExtended.bodyFrameData.bodyFrameDataMask &
                LOCATION_NAV_DATA_HAS_LONG_ACCEL_BIT) {
            out.bodyFrameData.bodyFrameDataMask |= LOCATION_NAV_DATA_HAS_LONG_ACCEL_BIT;
        }
        if (locationExtended.bodyFrameData.bodyFrameDataMask &
                LOCATION_NAV_DATA_HAS_LAT_ACCEL_BIT) {
            out.bodyFrameData.bodyFrameDataMask |= LOCATION_NAV_DATA_HAS_LAT_ACCEL_BIT;
        }
        if (locationExtended.bodyFrameData.bodyFrameDataMask &
                LOCATION_NAV_DATA_HAS_VERT_ACCEL_BIT) {
            out.bodyFrameData.bodyFrameDataMask |= LOCATION_NAV_DATA_HAS_VERT_ACCEL_BIT;
        }
        if (locationExtended.bodyFrameData.bodyFrameDataMask &
                LOCATION_NAV_DATA_HAS_YAW_RATE_BIT) {
            out.bodyFrameData.bodyFrameDataMask |= LOCATION_NAV_DATA_HAS_YAW_RATE_BIT;
        }
        if (locationExtended.bodyFrameData.bodyFrameDataMask &
            LOCATION_NAV_DATA_HAS_PITCH_BIT) {
            out.bodyFrameData.bodyFrameDataMask |= LOCATION_NAV_DATA_HAS_PITCH_BIT;
        }

        if (locationExtended.bodyFrameData.bodyFrameDataMask &
                LOCATION_NAV_DATA_HAS_LONG_ACCEL_UNC_BIT) {
            out.bodyFrameData.bodyFrameDataMask |= LOCATION_NAV_DATA_HAS_LONG_ACCEL_UNC_BIT;
        }
        if (locationExtended.bodyFrameData.bodyFrameDataMask &
                LOCATION_NAV_DATA_HAS_LAT_ACCEL_UNC_BIT) {
            out.bodyFrameData.bodyFrameDataMask |= LOCATION_NAV_DATA_HAS_LAT_ACCEL_UNC_BIT;
        }
        if (locationExtended.bodyFrameData.bodyFrameDataMask &
                LOCATION_NAV_DATA_HAS_VERT_ACCEL_UNC_BIT) {
            out.bodyFrameData.bodyFrameDataMask |= LOCATION_NAV_DATA_HAS_VERT_ACCEL_UNC_BIT;
        }
        if (locationExtended.bodyFrameData.bodyFrameDataMask &
                LOCATION_NAV_DATA_HAS_YAW_RATE_UNC_BIT) {
            out.bodyFrameData.bodyFrameDataMask |= LOCATION_NAV_DATA_HAS_YAW_RATE_UNC_BIT;
        }
        if (locationExtended.bodyFrameData.bodyFrameDataMask &
                LOCATION_NAV_DATA_HAS_PITCH_UNC_BIT) {
            out.bodyFrameData.bodyFrameDataMask |= LOCATION_NAV_DATA_HAS_PITCH_UNC_BIT;
        }

        if (locationExtended.bodyFrameDataExt.bodyFrameDataMask &
                LOCATION_NAV_DATA_HAS_PITCH_RATE_BIT) {
            out.bodyFrameDataExt.bodyFrameDataMask |= LOCATION_NAV_DATA_HAS_PITCH_RATE_BIT;
        }
        if (locationExtended.bodyFrameDataExt.bodyFrameDataMask &
                LOCATION_NAV_DATA_HAS_PITCH_RATE_UNC_BIT) {
            out.bodyFrameDataExt.bodyFrameDataMask |= LOCATION_NAV_DATA_HAS_PITCH_RATE_UNC_BIT;
        }
        if (locationExtended.bodyFrameDataExt.bodyFrameDataMask &
                LOCATION_NAV_DATA_HAS_ROLL_BIT) {
            out.bodyFrameDataExt.bodyFrameDataMask |= LOCATION_NAV_DATA_HAS_ROLL_BIT;
        }
        if (locationExtended.bodyFrameDataExt.bodyFrameDataMask &
                LOCATION_NAV_DATA_HAS_ROLL_UNC_BIT) {
            out.bodyFrameDataExt.bodyFrameDataMask |= LOCATION_NAV_DATA_HAS_ROLL_UNC_BIT;
        }
        if (locationExtended.bodyFrameDataExt.bodyFrameDataMask &
                LOCATION_NAV_DATA_HAS_ROLL_RATE_BIT) {
            out.bodyFrameDataExt.bodyFrameDataMask |= LOCATION_NAV_DATA_HAS_ROLL_RATE_BIT;
        }
        if (locationExtended.bodyFrameDataExt.bodyFrameDataMask &
                LOCATION_NAV_DATA_HAS_ROLL_RATE_UNC_BIT) {
            out.bodyFrameDataExt.bodyFrameDataMask |= LOCATION_NAV_DATA_HAS_ROLL_RATE_UNC_BIT;
        }
        if (locationExtended.bodyFrameDataExt.bodyFrameDataMask &
                LOCATION_NAV_DATA_HAS_YAW_BIT) {
            out.bodyFrameDataExt.bodyFrameDataMask |= LOCATION_NAV_DATA_HAS_YAW_BIT;
        }
        if (locationExtended.bodyFrameDataExt.bodyFrameDataMask &
                LOCATION_NAV_DATA_HAS_YAW_UNC_BIT) {
            out.bodyFrameDataExt.bodyFrameDataMask |= LOCATION_NAV_DATA_HAS_YAW_UNC_BIT;
        }

        out.bodyFrameData.longAccel = locationExtended.bodyFrameData.longAccel;
        out.bodyFrameData.latAccel = locationExtended.bodyFrameData.latAccel;
        out.bodyFrameData.vertAccel = locationExtended.bodyFrameData.vertAccel;
        out.bodyFrameData.yawRate = locationExtended.bodyFrameData.yawRate;
        out.bodyFrameData.pitch = locationExtended.bodyFrameData.pitch;
        out.bodyFrameData.longAccelUnc = locationExtended.bodyFrameData.longAccelUnc;
        out.bodyFrameData.latAccelUnc  = locationExtended.bodyFrameData.latAccelUnc;
        out.bodyFrameData.vertAccelUnc = locationExtended.bodyFrameData.vertAccelUnc;
        out.bodyFrameData.yawRateUnc   = locationExtended.bodyFrameData.yawRateUnc;
        out.bodyFrameData.pitchUnc     = locationExtended.bodyFrameData.pitchUnc;

        out.bodyFrameDataExt.pitchRate    = locationExtended.bodyFrameDataExt.pitchRate;
        out.bodyFrameDataExt.pitchRateUnc = locationExtended.bodyFrameDataExt.pitchRateUnc;
        out.bodyFrameDataExt.roll         = locationExtended.bodyFrameDataExt.roll;
        out.bodyFrameDataExt.rollUnc      = locationExtended.bodyFrameDataExt.rollUnc;
        out.bodyFrameDataExt.rollRate     = locationExtended.bodyFrameDataExt.rollRate;
        out.bodyFrameDataExt.rollRateUnc  = locationExtended.bodyFrameDataExt.rollRateUnc;
        out.bodyFrameDataExt.yaw          = locationExtended.bodyFrameDataExt.yaw;
        out.bodyFrameDataExt.yawUnc       = locationExtended.bodyFrameDataExt.yawUnc;
    }

    // Validity of this structure is established from the timeSrc of the GnssSystemTime structure.
    out.gnssSystemTime = locationExtended.gnssSystemTime;

    if (GPS_LOCATION_EXTENDED_HAS_LEAP_SECONDS & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_LEAP_SECONDS_BIT;
        out.leapSeconds = locationExtended.leapSeconds;
    }

    if (GPS_LOCATION_EXTENDED_HAS_TIME_UNC & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_TIME_UNC_BIT;
        out.timeUncMs = locationExtended.timeUncMs;
    }

    if (GPS_LOCATION_EXTENDED_HAS_CALIBRATION_CONFIDENCE & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_CALIBRATION_CONFIDENCE_BIT;
        out.calibrationConfidence = locationExtended.calibrationConfidence;
    }

    if (GPS_LOCATION_EXTENDED_HAS_CALIBRATION_STATUS & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_CALIBRATION_STATUS_BIT;
        out.calibrationStatus = locationExtended.calibrationStatus;
    }

    if (GPS_LOCATION_EXTENDED_HAS_OUTPUT_ENG_TYPE & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_OUTPUT_ENG_TYPE_BIT;
        out.locOutputEngType = locationExtended.locOutputEngType;
    }

    if (GPS_LOCATION_EXTENDED_HAS_OUTPUT_ENG_MASK & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_OUTPUT_ENG_MASK_BIT;
        out.locOutputEngMask = locationExtended.locOutputEngMask;
    }

    if (GPS_LOCATION_EXTENDED_HAS_CONFORMITY_INDEX & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_CONFORMITY_INDEX_BIT;
        out.conformityIndex = locationExtended.conformityIndex;
    }

    if (GPS_LOCATION_EXTENDED_HAS_LLA_VRP_BASED & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_LLA_VRP_BASED_BIT;
        out.llaVRPBased = locationExtended.llaVRPBased;
    }

    if (GPS_LOCATION_EXTENDED_HAS_ENU_VELOCITY_LLA_VRP_BASED & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_ENU_VELOCITY_VRP_BASED_BIT;
        // copy over east, north and up vrp based velocity
        out.enuVelocityVRPBased[0] = locationExtended.enuVelocityVRPBased[0];
        out.enuVelocityVRPBased[1] = locationExtended.enuVelocityVRPBased[1];
        out.enuVelocityVRPBased[2] = locationExtended.enuVelocityVRPBased[2];
    }

    if (GPS_LOCATION_EXTENDED_HAS_DR_SOLUTION_STATUS_MASK & locationExtended.flags) {
        out.flags |= GNSS_LOCATION_INFO_DR_SOLUTION_STATUS_MASK_BIT;
        out.drSolutionStatusMask = locationExtended.drSolutionStatusMask;
    }
}

inline uint32_t
GnssAdapter::convertSuplVersion(const GnssConfigSuplVersion suplVersion)
{
    switch (suplVersion) {
        case GNSS_CONFIG_SUPL_VERSION_2_0_0:
            return 0x00020000;
        case GNSS_CONFIG_SUPL_VERSION_2_0_2:
            return 0x00020002;
        case GNSS_CONFIG_SUPL_VERSION_1_0_0:
        default:
            return 0x00010000;
    }
}

inline uint32_t
GnssAdapter::convertLppProfile(const GnssConfigLppProfile lppProfile)
{
    switch (lppProfile) {
        case GNSS_CONFIG_LPP_PROFILE_USER_PLANE:
            return 1;
        case GNSS_CONFIG_LPP_PROFILE_CONTROL_PLANE:
            return 2;
        case GNSS_CONFIG_LPP_PROFILE_USER_PLANE_AND_CONTROL_PLANE:
            return 3;
        case GNSS_CONFIG_LPP_PROFILE_RRLP_ON_LTE:
        default:
            return 0;
    }
}

uint32_t
GnssAdapter::convertLppeCp(const GnssConfigLppeControlPlaneMask lppeControlPlaneMask)
{
    uint32_t mask = 0;
    if (GNSS_CONFIG_LPPE_CONTROL_PLANE_DBH_BIT & lppeControlPlaneMask) {
        mask |= (1<<0);
    }
    if (GNSS_CONFIG_LPPE_CONTROL_PLANE_WLAN_AP_MEASUREMENTS_BIT & lppeControlPlaneMask) {
        mask |= (1<<1);
    }
    if (GNSS_CONFIG_LPPE_CONTROL_PLANE_SRN_AP_MEASUREMENTS_BIT & lppeControlPlaneMask) {
        mask |= (1<<2);
    }
    if (GNSS_CONFIG_LPPE_CONTROL_PLANE_SENSOR_BARO_MEASUREMENTS_BIT & lppeControlPlaneMask) {
        mask |= (1<<3);
    }
    return mask;
}

uint32_t
GnssAdapter::convertLppeUp(const GnssConfigLppeUserPlaneMask lppeUserPlaneMask)
{
    uint32_t mask = 0;
    if (GNSS_CONFIG_LPPE_USER_PLANE_DBH_BIT & lppeUserPlaneMask) {
        mask |= (1<<0);
    }
    if (GNSS_CONFIG_LPPE_USER_PLANE_WLAN_AP_MEASUREMENTS_BIT & lppeUserPlaneMask) {
        mask |= (1<<1);
    }
    if (GNSS_CONFIG_LPPE_USER_PLANE_SRN_AP_MEASUREMENTS_BIT & lppeUserPlaneMask) {
        mask |= (1<<2);
    }
    if (GNSS_CONFIG_LPPE_USER_PLANE_SENSOR_BARO_MEASUREMENTS_BIT & lppeUserPlaneMask) {
        mask |= (1<<3);
    }
    return mask;
}

uint32_t
GnssAdapter::convertAGloProt(const GnssConfigAGlonassPositionProtocolMask aGloPositionProtocolMask)
{
    uint32_t mask = 0;
    if (GNSS_CONFIG_RRC_CONTROL_PLANE_BIT & aGloPositionProtocolMask) {
        mask |= (1<<0);
    }
    if (GNSS_CONFIG_RRLP_USER_PLANE_BIT & aGloPositionProtocolMask) {
        mask |= (1<<1);
    }
    if (GNSS_CONFIG_LLP_USER_PLANE_BIT & aGloPositionProtocolMask) {
        mask |= (1<<2);
    }
    if (GNSS_CONFIG_LLP_CONTROL_PLANE_BIT & aGloPositionProtocolMask) {
        mask |= (1<<3);
    }
    return mask;
}

uint32_t
GnssAdapter::convertEP4ES(const GnssConfigEmergencyPdnForEmergencySupl emergencyPdnForEmergencySupl)
{
    switch (emergencyPdnForEmergencySupl) {
       case GNSS_CONFIG_EMERGENCY_PDN_FOR_EMERGENCY_SUPL_YES:
           return 1;
       case GNSS_CONFIG_EMERGENCY_PDN_FOR_EMERGENCY_SUPL_NO:
       default:
           return 0;
    }
}

uint32_t
GnssAdapter::convertSuplEs(const GnssConfigSuplEmergencyServices suplEmergencyServices)
{
    switch (suplEmergencyServices) {
       case GNSS_CONFIG_SUPL_EMERGENCY_SERVICES_YES:
           return 1;
       case GNSS_CONFIG_SUPL_EMERGENCY_SERVICES_NO:
       default:
           return 0;
    }
}

uint32_t
GnssAdapter::convertSuplMode(const GnssConfigSuplModeMask suplModeMask)
{
    uint32_t mask = 0;
    if (GNSS_CONFIG_SUPL_MODE_MSB_BIT & suplModeMask) {
        mask |= (1<<0);
    }
    if (GNSS_CONFIG_SUPL_MODE_MSA_BIT & suplModeMask) {
        mask |= (1<<1);
    }
    return mask;
}

void
GnssAdapter::readConfigCommand()
{
    LOC_LOGD("%s]: ", __func__);

    struct MsgReadConfig : public LocMsg {
        GnssAdapter* mAdapter;
        ContextBase& mContext;
        inline MsgReadConfig(GnssAdapter* adapter,
                             ContextBase& context) :
            LocMsg(),
            mAdapter(adapter),
            mContext(context) {}
        inline virtual void proc() const {
            // reads config into mContext->mGps_conf
            mContext.readConfig();
        }
    };

    if (mContext != NULL) {
        sendMsg(new MsgReadConfig(this, *mContext));
    }
}

void
GnssAdapter::setSuplHostServer(const char* server, int port, LocServerType type)
{
    if (ContextBase::mGps_conf.AGPS_CONFIG_INJECT) {
        char serverUrl[MAX_URL_LEN] = {};
        int32_t length = -1;
        const char noHost[] = "NONE";

        if ((NULL == server) || (server[0] == 0) ||
                (strncasecmp(noHost, server, sizeof(noHost)) == 0)) {
            serverUrl[0] = '\0';
            length = 0;
        } else if (port > 0) {
            length = snprintf(serverUrl, sizeof(serverUrl), "%s:%u", server, port);
        }
        if (LOC_AGPS_SUPL_SERVER != type && LOC_AGPS_MO_SUPL_SERVER != type) {
            LOC_LOGe("Invalid type=%d", type);
        } else {
            string& url = (LOC_AGPS_SUPL_SERVER == type) ? getServerUrl() : getMoServerUrl();
            if (length > 0 && strncasecmp(url.c_str(), serverUrl, sizeof(serverUrl)) != 0) {
                url.assign(serverUrl);

                if (LOC_AGPS_SUPL_SERVER == type) {
                    strlcpy(ContextBase::mGps_conf.SUPL_HOST, server, LOC_MAX_PARAM_STRING);
                    ContextBase::mGps_conf.SUPL_PORT = port;
                }

                LOC_LOGv("serverUrl=%s length=%d type=%d", serverUrl, length, type);
            }
        }
    }
}

void
GnssAdapter::setConfigCommand()
{
    LOC_LOGD("%s]: ", __func__);

    struct MsgSetConfig : public LocMsg {
        GnssAdapter& mAdapter;
        LocApiBase& mApi;
        inline MsgSetConfig(GnssAdapter& adapter, LocApiBase& api) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api) {}
        inline virtual void proc() const {

            // set nmea mask type
            uint32_t mask = 0;
            if (NMEA_PROVIDER_MP == ContextBase::mGps_conf.NMEA_PROVIDER) {
                mask |= LOC_NMEA_ALL_GENERAL_SUPPORTED_MASK;
            }
            if (ContextBase::isFeatureSupported(LOC_SUPPORTED_FEATURE_DEBUG_NMEA_V02)) {
                mask |= LOC_NMEA_MASK_DEBUG_V02;
            }
            mAdapter.mNmeaMask= mask;

            std::string oldServerUrl = mAdapter.getServerUrl();
            std::string oldMoServerUrl = mAdapter.getMoServerUrl();
            mAdapter.setSuplHostServer(ContextBase::mGps_conf.SUPL_HOST,
                                       ContextBase::mGps_conf.SUPL_PORT,
                                       LOC_AGPS_SUPL_SERVER);
            mAdapter.setSuplHostServer(ContextBase::mGps_conf.MO_SUPL_HOST,
                                       ContextBase::mGps_conf.MO_SUPL_PORT,
                                       LOC_AGPS_MO_SUPL_SERVER);

           // inject the configurations into modem
           GnssAdapter& adapter = mAdapter;
           loc_gps_cfg_s gpsConf = ContextBase::mGps_conf;
           loc_sap_cfg_s_type sapConf = ContextBase::mSap_conf;

            mApi.sendMsg(new LocApiMsg(
                    [&adapter, gpsConf, sapConf, oldServerUrl, oldMoServerUrl] () {
                //cache the injected configuration with GnssConfigRequested struct
                GnssConfig gnssConfigRequested = {};
                gnssConfigRequested.flags |= GNSS_CONFIG_FLAGS_GPS_LOCK_VALID_BIT |
                        GNSS_CONFIG_FLAGS_LPPE_CONTROL_PLANE_VALID_BIT |
                        GNSS_CONFIG_FLAGS_LPPE_USER_PLANE_VALID_BIT |
                        GNSS_CONFIG_FLAGS_BLACKLISTED_SV_IDS_BIT;
                gnssConfigRequested.gpsLock = GNSS_CONFIG_GPS_LOCK_NONE;
                if (0 == adapter.getPowerVoteId()) {
                    gnssConfigRequested.gpsLock = gpsConf.GPS_LOCK;
                }

                if (gpsConf.AGPS_CONFIG_INJECT) {
                    gnssConfigRequested.flags |= GNSS_CONFIG_FLAGS_SET_ASSISTANCE_DATA_VALID_BIT |
                            GNSS_CONFIG_FLAGS_SUPL_VERSION_VALID_BIT |
                            GNSS_CONFIG_FLAGS_AGLONASS_POSITION_PROTOCOL_VALID_BIT |
                            GNSS_CONFIG_FLAGS_LPP_PROFILE_VALID_BIT;
                    gnssConfigRequested.suplVersion =
                            adapter.mLocApi->convertSuplVersion(gpsConf.SUPL_VER);
                    gnssConfigRequested.lppProfile =
                            adapter.mLocApi->convertLppProfile(gpsConf.LPP_PROFILE);
                    gnssConfigRequested.aGlonassPositionProtocolMask =
                            gpsConf.A_GLONASS_POS_PROTOCOL_SELECT;
                }
                gnssConfigRequested.lppeControlPlaneMask =
                        adapter.mLocApi->convertLppeCp(gpsConf.LPPE_CP_TECHNOLOGY);
                gnssConfigRequested.lppeUserPlaneMask =
                        adapter.mLocApi->convertLppeUp(gpsConf.LPPE_UP_TECHNOLOGY);
                gnssConfigRequested.blacklistedSvIds.assign(adapter.mBlacklistedSvIds.begin(),
                        adapter.mBlacklistedSvIds.end());
                adapter.gnssUpdateConfig(oldServerUrl, oldMoServerUrl, gnssConfigRequested,
                        gnssConfigRequested);

                // set nmea mask type
                uint32_t mask = 0;
                if (NMEA_PROVIDER_MP == gpsConf.NMEA_PROVIDER) {
                    mask |= LOC_NMEA_ALL_GENERAL_SUPPORTED_MASK;
                }
                if (ContextBase::isFeatureSupported(LOC_SUPPORTED_FEATURE_DEBUG_NMEA_V02)) {
                    mask |= LOC_NMEA_MASK_DEBUG_V02;
                }

                if (mask != 0) {
                    adapter.mLocApi->setNMEATypesSync(mask);
                }

                adapter.mLocApi->setXtraVersionCheckSync(gpsConf.XTRA_VERSION_CHECK);

                // load tunc configuration from config file on first boot-up,
                // e.g.: adapter.mLocConfigInfo.tuncConfigInfo.isValid is false
                if (adapter.mLocConfigInfo.tuncConfigInfo.isValid == false) {
                    adapter.mLocConfigInfo.tuncConfigInfo.isValid = true;
                    adapter.mLocConfigInfo.tuncConfigInfo.enable =
                        (gpsConf.CONSTRAINED_TIME_UNCERTAINTY_ENABLED == 1);
                    adapter.mLocConfigInfo.tuncConfigInfo.tuncThresholdMs =
                        (float)gpsConf.CONSTRAINED_TIME_UNCERTAINTY_THRESHOLD;
                    adapter.mLocConfigInfo.tuncConfigInfo.energyBudget =
                        gpsConf.CONSTRAINED_TIME_UNCERTAINTY_ENERGY_BUDGET;
                }

                adapter.mLocApi->setConstrainedTuncMode(
                        adapter.mLocConfigInfo.tuncConfigInfo.enable,
                        adapter.mLocConfigInfo.tuncConfigInfo.tuncThresholdMs,
                        adapter.mLocConfigInfo.tuncConfigInfo.energyBudget);

                // load pace configuration from config file on first boot-up,
                // e.g.: adapter.mLocConfigInfo.paceConfigInfo.isValid is false
                if (adapter.mLocConfigInfo.paceConfigInfo.isValid == false) {
                    adapter.mLocConfigInfo.paceConfigInfo.isValid = true;
                    adapter.mLocConfigInfo.paceConfigInfo.enable =
                        (gpsConf.POSITION_ASSISTED_CLOCK_ESTIMATOR_ENABLED==1);
                }
                adapter.mLocApi->setPositionAssistedClockEstimatorMode(
                        adapter.mLocConfigInfo.paceConfigInfo.enable);

                // we do not support control robust location from gps.conf
                if (adapter.mLocConfigInfo.robustLocationConfigInfo.isValid == true) {
                    adapter.mLocApi->configRobustLocation(
                            adapter.mLocConfigInfo.robustLocationConfigInfo.enable,
                            adapter.mLocConfigInfo.robustLocationConfigInfo.enableFor911);
                }

                if (sapConf.GYRO_BIAS_RANDOM_WALK_VALID ||
                    sapConf.ACCEL_RANDOM_WALK_SPECTRAL_DENSITY_VALID ||
                    sapConf.ANGLE_RANDOM_WALK_SPECTRAL_DENSITY_VALID ||
                    sapConf.RATE_RANDOM_WALK_SPECTRAL_DENSITY_VALID ||
                    sapConf.VELOCITY_RANDOM_WALK_SPECTRAL_DENSITY_VALID ) {
                    adapter.mLocApi->setSensorPropertiesSync(
                        sapConf.GYRO_BIAS_RANDOM_WALK_VALID,
                        sapConf.GYRO_BIAS_RANDOM_WALK,
                        sapConf.ACCEL_RANDOM_WALK_SPECTRAL_DENSITY_VALID,
                        sapConf.ACCEL_RANDOM_WALK_SPECTRAL_DENSITY,
                        sapConf.ANGLE_RANDOM_WALK_SPECTRAL_DENSITY_VALID,
                        sapConf.ANGLE_RANDOM_WALK_SPECTRAL_DENSITY,
                        sapConf.RATE_RANDOM_WALK_SPECTRAL_DENSITY_VALID,
                        sapConf.RATE_RANDOM_WALK_SPECTRAL_DENSITY,
                        sapConf.VELOCITY_RANDOM_WALK_SPECTRAL_DENSITY_VALID,
                        sapConf.VELOCITY_RANDOM_WALK_SPECTRAL_DENSITY);
                }
                adapter.mLocApi->setSensorPerfControlConfigSync(
                    sapConf.SENSOR_CONTROL_MODE,
                    sapConf.SENSOR_ACCEL_SAMPLES_PER_BATCH,
                    sapConf.SENSOR_ACCEL_BATCHES_PER_SEC,
                    sapConf.SENSOR_GYRO_SAMPLES_PER_BATCH,
                    sapConf.SENSOR_GYRO_BATCHES_PER_SEC,
                    sapConf.SENSOR_ACCEL_SAMPLES_PER_BATCH_HIGH,
                    sapConf.SENSOR_ACCEL_BATCHES_PER_SEC_HIGH,
                    sapConf.SENSOR_GYRO_SAMPLES_PER_BATCH_HIGH,
                    sapConf.SENSOR_GYRO_BATCHES_PER_SEC_HIGH,
                    sapConf.SENSOR_ALGORITHM_CONFIG_MASK);
            } ));
        }
    };

    sendMsg(new MsgSetConfig(*this, *mLocApi));
}

std::vector<LocationError> GnssAdapter::gnssUpdateConfig(const std::string& oldServerUrl,
        const std::string& oldMoServerUrl, const GnssConfig& gnssConfigRequested,
        const GnssConfig& gnssConfigNeedEngineUpdate, size_t count) {
    loc_gps_cfg_s gpsConf = ContextBase::mGps_conf;
    size_t index = 0;
    LocationError err = LOCATION_ERROR_SUCCESS;
    std::vector<LocationError> errsList = {err};
    if (count > 0) {
        errsList.insert(errsList.begin(), count, LOCATION_ERROR_SUCCESS);
    }


    std::string serverUrl = getServerUrl();
    std::string moServerUrl = getMoServerUrl();

    int serverUrlLen = serverUrl.length();
    int moServerUrlLen = moServerUrl.length();

    if (gnssConfigRequested.flags & GNSS_CONFIG_FLAGS_GPS_LOCK_VALID_BIT) {
        if (gnssConfigNeedEngineUpdate.flags & GNSS_CONFIG_FLAGS_GPS_LOCK_VALID_BIT) {
            err = mLocApi->setGpsLockSync(gnssConfigRequested.gpsLock);
            if (index < count) {
                errsList[index] = err;
            }
        }
        index++;
    }

    if (gnssConfigRequested.flags &
            GNSS_CONFIG_FLAGS_SET_ASSISTANCE_DATA_VALID_BIT) {
        if (gnssConfigNeedEngineUpdate.flags &
                GNSS_CONFIG_FLAGS_SET_ASSISTANCE_DATA_VALID_BIT) {
            if (gnssConfigNeedEngineUpdate.assistanceServer.type ==
                    GNSS_ASSISTANCE_TYPE_SUPL) {
                if ((serverUrlLen != 0) && (oldServerUrl.compare(serverUrl) !=0)) {

                    err = mLocApi->setServerSync(
                            serverUrl.c_str(), serverUrlLen, LOC_AGPS_SUPL_SERVER);
                    if (index < count) {
                        errsList[index] = err;
                    }
                }
                if ((moServerUrlLen != 0) && (oldMoServerUrl.compare(moServerUrl) != 0)) {
                    LocationError locErr =
                        mLocApi->setServerSync(moServerUrl.c_str(),
                                moServerUrlLen,
                                LOC_AGPS_MO_SUPL_SERVER);
                    if (locErr != LOCATION_ERROR_SUCCESS) {
                        LOC_LOGe("Error while setting MO SUPL_HOST server:%s",
                                moServerUrl.c_str());
                    }
                }
            } else if (gnssConfigNeedEngineUpdate.assistanceServer.type ==
                    GNSS_ASSISTANCE_TYPE_C2K) {
                struct in_addr addr;
                struct hostent* hp;
                bool resolveAddrSuccess = true;

                hp = gethostbyname(
                        gnssConfigNeedEngineUpdate.assistanceServer.hostName);
                if (hp != NULL) { /* DNS OK */
                    memcpy(&addr, hp->h_addr_list[0], hp->h_length);
                } else {
                    /* Try IP representation */
                    if (inet_aton(
                                gnssConfigNeedEngineUpdate.assistanceServer.hostName,
                                &addr) == 0) {
                        /* IP not valid */
                        LOC_LOGE("%s]: hostname '%s' cannot be resolved ",
                                __func__,
                                gnssConfigNeedEngineUpdate.assistanceServer.hostName);
                        if (index < count) {
                            errsList[index] = LOCATION_ERROR_INVALID_PARAMETER;
                        }
                    } else {
                        resolveAddrSuccess = false;
                    }
                }

                if (resolveAddrSuccess) {
                    unsigned int ip = htonl(addr.s_addr);
                    err = mLocApi->setServerSync(ip,
                            gnssConfigNeedEngineUpdate.assistanceServer.port,
                            LOC_AGPS_CDMA_PDE_SERVER);
                    if (index < count) {
                        errsList[index] = err;
                    }
                }
            }
        }
        index++;
    }

    if (gnssConfigRequested.flags & GNSS_CONFIG_FLAGS_SUPL_VERSION_VALID_BIT) {
        if (gnssConfigNeedEngineUpdate.flags &
                GNSS_CONFIG_FLAGS_SUPL_VERSION_VALID_BIT) {
            err = mLocApi->setSUPLVersionSync(gnssConfigRequested.suplVersion);
            if (index < count) {
                errsList[index] = err;
            }
        }
        index++;
    }

    if (gnssConfigRequested.flags & GNSS_CONFIG_FLAGS_LPP_PROFILE_VALID_BIT) {
        if (gnssConfigNeedEngineUpdate.flags &
                GNSS_CONFIG_FLAGS_LPP_PROFILE_VALID_BIT) {
            err = mLocApi->setLPPConfigSync(gnssConfigRequested.lppProfile);
            if (index < count) {
                errsList[index] = err;
            }
        }
        index++;
    }

    if (gnssConfigRequested.flags & GNSS_CONFIG_FLAGS_LPPE_CONTROL_PLANE_VALID_BIT) {
        if (gnssConfigNeedEngineUpdate.flags &
                GNSS_CONFIG_FLAGS_LPPE_CONTROL_PLANE_VALID_BIT) {
            err = mLocApi->setLPPeProtocolCpSync(
                    gnssConfigRequested.lppeControlPlaneMask);
            if (index < count) {
                errsList[index] = err;
            }
        }
        index++;
    }

    if (gnssConfigRequested.flags & GNSS_CONFIG_FLAGS_LPPE_USER_PLANE_VALID_BIT) {
        if (gnssConfigNeedEngineUpdate.flags &
                GNSS_CONFIG_FLAGS_LPPE_USER_PLANE_VALID_BIT) {
            err = mLocApi->setLPPeProtocolUpSync(
                    gnssConfigRequested.lppeUserPlaneMask);
            if (index < count) {
                errsList[index] = err;
            }
        }
        index++;
    }

    if (gnssConfigRequested.flags &
            GNSS_CONFIG_FLAGS_AGLONASS_POSITION_PROTOCOL_VALID_BIT) {
        if (gnssConfigNeedEngineUpdate.flags &
                GNSS_CONFIG_FLAGS_AGLONASS_POSITION_PROTOCOL_VALID_BIT) {
            err = mLocApi->setAGLONASSProtocolSync(
                    gnssConfigRequested.aGlonassPositionProtocolMask);
            if (index < count) {
                errsList[index] = err;
            }
        }
        index++;
    }
    if (gnssConfigRequested.flags & GNSS_CONFIG_FLAGS_BLACKLISTED_SV_IDS_BIT) {
        // Check if feature is supported
        if (!ContextBase::isFeatureSupported(
                    LOC_SUPPORTED_FEATURE_CONSTELLATION_ENABLEMENT_V02)) {
            LOC_LOGe("Feature constellation enablement not supported.");
            err = LOCATION_ERROR_NOT_SUPPORTED;
        } else {
            // Send the SV ID Config to Modem
            mBlacklistedSvIds.assign(gnssConfigRequested.blacklistedSvIds.begin(),
                    gnssConfigRequested.blacklistedSvIds.end());
            err = gnssSvIdConfigUpdateSync(gnssConfigRequested.blacklistedSvIds);
            if (LOCATION_ERROR_SUCCESS != err) {
                LOC_LOGe("Failed to send config to modem, err %d", err);
            }
        }
        if (index < count) {
            errsList[index] = err;
        }
        index++;
    }

    if (gnssConfigRequested.flags & GNSS_CONFIG_FLAGS_MIN_SV_ELEVATION_BIT) {
        GnssConfig gnssConfig = {};
        gnssConfig.flags = GNSS_CONFIG_FLAGS_MIN_SV_ELEVATION_BIT;
        gnssConfig.minSvElevation = gnssConfigRequested.minSvElevation;
        err = mLocApi->setParameterSync(gnssConfig);
        if (index < count) {
            errsList[index] = err;
        }
        index++;
    }

    return errsList;
}

uint32_t*
GnssAdapter::gnssUpdateConfigCommand(const GnssConfig& config)
{
    // count the number of bits set
    GnssConfigFlagsMask flagsCopy = config.flags;
    size_t count = 0;
    while (flagsCopy > 0) {
        if (flagsCopy & 1) {
            count++;
        }
        flagsCopy >>= 1;
    }
    std::string idsString = "[";
    uint32_t* ids = NULL;
    if (count > 0) {
        ids = new uint32_t[count];
        if (ids == nullptr) {
            LOC_LOGE("%s] new allocation failed, fatal error.", __func__);
            return nullptr;
        }
        for (size_t i=0; i < count; ++i) {
            ids[i] = generateSessionId();
            IF_LOC_LOGD {
                idsString += std::to_string(ids[i]) + " ";
            }
        }
    }
    idsString += "]";

    LOC_LOGD("%s]: ids %s flags 0x%X", __func__, idsString.c_str(), config.flags);

    struct MsgGnssUpdateConfig : public LocMsg {
        GnssAdapter& mAdapter;
        LocApiBase& mApi;
        GnssConfig mConfig;
        size_t mCount;
        uint32_t* mIds;
        inline MsgGnssUpdateConfig(GnssAdapter& adapter,
                                   LocApiBase& api,
                                   GnssConfig config,
                                   uint32_t* ids,
                                   size_t count) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api),
            mConfig(config),
            mCount(count),
            mIds(nullptr) {
                if (mCount > 0) {
                    mIds = new uint32_t[count];
                    if (mIds) {
                        for (uint32_t index = 0; index < count; index++) {
                            mIds[index] = ids[index];
                        }
                    } else {
                        LOC_LOGe("memory allocation for mIds failed");
                    }
                }
           }

        inline virtual ~MsgGnssUpdateConfig()
        {
            delete [] mIds;
        }

        inline virtual void proc() const {

            GnssAdapter& adapter = mAdapter;
            size_t countOfConfigs = mCount;
            GnssConfig gnssConfigRequested = mConfig;
            GnssConfig gnssConfigNeedEngineUpdate = mConfig;
            std::string oldServerUrl = mAdapter.getServerUrl();

            std::vector<uint32_t> sessionIds;
            sessionIds.assign(mIds, mIds + mCount);
            std::vector<LocationError> errs(mCount, LOCATION_ERROR_SUCCESS);
            int index = 0;
            bool needSuspendResume = false;

            if (gnssConfigRequested.flags & GNSS_CONFIG_FLAGS_GPS_LOCK_VALID_BIT) {
                GnssConfigGpsLock newGpsLock = gnssConfigRequested.gpsLock;
                if (GNSS_CONFIG_GPS_LOCK_NONE == newGpsLock) {
                    newGpsLock = GNSS_CONFIG_GPS_LOCK_MO_AND_NI;
                }
                if (newGpsLock == ContextBase::mGps_conf.GPS_LOCK ||
                        0 != mAdapter.getPowerVoteId()) {
                    gnssConfigNeedEngineUpdate.flags &= ~(GNSS_CONFIG_FLAGS_GPS_LOCK_VALID_BIT);
                }
                ContextBase::mGps_conf.GPS_LOCK = newGpsLock;
                index++;
            }
            if (gnssConfigRequested.flags & GNSS_CONFIG_FLAGS_SUPL_VERSION_VALID_BIT) {
                uint32_t newSuplVersion =
                        mAdapter.convertSuplVersion(gnssConfigRequested.suplVersion);
                if (newSuplVersion != ContextBase::mGps_conf.SUPL_VER &&
                    ContextBase::mGps_conf.AGPS_CONFIG_INJECT) {
                    ContextBase::mGps_conf.SUPL_VER = newSuplVersion;
                } else {
                    gnssConfigNeedEngineUpdate.flags &= ~(GNSS_CONFIG_FLAGS_SUPL_VERSION_VALID_BIT);
                }
                index++;
            }
            if (gnssConfigRequested.flags & GNSS_CONFIG_FLAGS_SET_ASSISTANCE_DATA_VALID_BIT) {
                if (GNSS_ASSISTANCE_TYPE_SUPL == mConfig.assistanceServer.type) {
                    mAdapter.setSuplHostServer(mConfig.assistanceServer.hostName,
                                                     mConfig.assistanceServer.port,
                                                     LOC_AGPS_SUPL_SERVER);
                } else {
                    LOC_LOGE("%s]: Not a valid gnss assistance type %u",
                            __func__, mConfig.assistanceServer.type);
                    errs.at(index) = LOCATION_ERROR_INVALID_PARAMETER;
                    gnssConfigNeedEngineUpdate.flags &=
                            ~(GNSS_CONFIG_FLAGS_SET_ASSISTANCE_DATA_VALID_BIT);
                }
                index++;
            }
            if (gnssConfigRequested.flags & GNSS_CONFIG_FLAGS_LPP_PROFILE_VALID_BIT) {
                uint32_t newLppProfile = mAdapter.convertLppProfile(gnssConfigRequested.lppProfile);
                if (newLppProfile != ContextBase::mGps_conf.LPP_PROFILE &&
                    ContextBase::mGps_conf.AGPS_CONFIG_INJECT) {
                    ContextBase::mGps_conf.LPP_PROFILE = newLppProfile;
                } else {
                    gnssConfigNeedEngineUpdate.flags &= ~(GNSS_CONFIG_FLAGS_LPP_PROFILE_VALID_BIT);
                }
                index++;
            }
            if (gnssConfigRequested.flags & GNSS_CONFIG_FLAGS_LPPE_CONTROL_PLANE_VALID_BIT) {
                uint32_t newLppeControlPlaneMask =
                    mAdapter.convertLppeCp(gnssConfigRequested.lppeControlPlaneMask);
                if (newLppeControlPlaneMask != ContextBase::mGps_conf.LPPE_CP_TECHNOLOGY) {
                    ContextBase::mGps_conf.LPPE_CP_TECHNOLOGY = newLppeControlPlaneMask;
                } else {
                    gnssConfigNeedEngineUpdate.flags &=
                            ~(GNSS_CONFIG_FLAGS_LPPE_CONTROL_PLANE_VALID_BIT);
                }
                index++;
            }
            if (gnssConfigRequested.flags & GNSS_CONFIG_FLAGS_LPPE_USER_PLANE_VALID_BIT) {
                uint32_t newLppeUserPlaneMask =
                    mAdapter.convertLppeUp(gnssConfigRequested.lppeUserPlaneMask);
                if (newLppeUserPlaneMask != ContextBase::mGps_conf.LPPE_UP_TECHNOLOGY) {
                    ContextBase::mGps_conf.LPPE_UP_TECHNOLOGY = newLppeUserPlaneMask;
                } else {
                    gnssConfigNeedEngineUpdate.flags &=
                            ~(GNSS_CONFIG_FLAGS_LPPE_USER_PLANE_VALID_BIT);
                }
                index++;
            }
            if (gnssConfigRequested.flags &
                    GNSS_CONFIG_FLAGS_AGLONASS_POSITION_PROTOCOL_VALID_BIT) {
                uint32_t newAGloProtMask =
                    mAdapter.convertAGloProt(gnssConfigRequested.aGlonassPositionProtocolMask);
                if (newAGloProtMask != ContextBase::mGps_conf.A_GLONASS_POS_PROTOCOL_SELECT &&
                    ContextBase::mGps_conf.AGPS_CONFIG_INJECT) {
                    ContextBase::mGps_conf.A_GLONASS_POS_PROTOCOL_SELECT = newAGloProtMask;
                } else {
                    gnssConfigNeedEngineUpdate.flags &=
                            ~(GNSS_CONFIG_FLAGS_AGLONASS_POSITION_PROTOCOL_VALID_BIT);
                }
                index++;
             }
             if (gnssConfigRequested.flags & GNSS_CONFIG_FLAGS_EM_PDN_FOR_EM_SUPL_VALID_BIT) {
                uint32_t newEP4ES = mAdapter.convertEP4ES(
                        gnssConfigRequested.emergencyPdnForEmergencySupl);
                if (newEP4ES != ContextBase::mGps_conf.USE_EMERGENCY_PDN_FOR_EMERGENCY_SUPL) {
                    ContextBase::mGps_conf.USE_EMERGENCY_PDN_FOR_EMERGENCY_SUPL = newEP4ES;
                }
                index++;
             }
             if (gnssConfigRequested.flags & GNSS_CONFIG_FLAGS_SUPL_EM_SERVICES_BIT) {
                uint32_t newSuplEs = mAdapter.convertSuplEs(
                        gnssConfigRequested.suplEmergencyServices);
                if (newSuplEs != ContextBase::mGps_conf.SUPL_ES) {
                    ContextBase::mGps_conf.SUPL_ES = newSuplEs;
                }
                index++;
             }
             if (gnssConfigRequested.flags & GNSS_CONFIG_FLAGS_SUPL_MODE_BIT) {
                uint32_t newSuplMode = mAdapter.convertSuplMode(gnssConfigRequested.suplModeMask);
                if (newSuplMode != ContextBase::mGps_conf.SUPL_MODE) {
                    ContextBase::mGps_conf.SUPL_MODE = newSuplMode;
                    mAdapter.broadcastCapabilities(mAdapter.getCapabilities());
                }
                index++;
            }

            if (gnssConfigRequested.flags & GNSS_CONFIG_FLAGS_MIN_SV_ELEVATION_BIT) {
                needSuspendResume = true;
                index++;
            }

            if (needSuspendResume == true) {
                mAdapter.suspendSessions();
            }
            LocApiCollectiveResponse *configCollectiveResponse = new LocApiCollectiveResponse(
                    *adapter.getContext(),
                    [&adapter, sessionIds, countOfConfigs] (std::vector<LocationError> errs) {

                    std::vector<uint32_t> ids(sessionIds);
                    adapter.reportResponse(countOfConfigs, errs.data(), ids.data());
            });

            mApi.sendMsg(new LocApiMsg(
                    [&adapter, gnssConfigRequested, gnssConfigNeedEngineUpdate,
                    countOfConfigs, configCollectiveResponse, errs, oldServerUrl] () {
                std::string serverUrl = adapter.getServerUrl();
                int serverUrlLen = serverUrl.length();
                std::vector<LocationError> errsList = adapter.gnssUpdateConfig(oldServerUrl,
                        "", gnssConfigRequested, gnssConfigNeedEngineUpdate, countOfConfigs);

                configCollectiveResponse->returnToSender(errsList);
            }));

            if (needSuspendResume == true) {
                mAdapter.restartSessions();
            }
        }
    };

    if (NULL != ids) {
        sendMsg(new MsgGnssUpdateConfig(*this, *mLocApi, config, ids, count));
    } else {
        LOC_LOGE("%s]: No GNSS config items to update", __func__);
    }

    return ids;
}

void
GnssAdapter::gnssSvIdConfigUpdate(const std::vector<GnssSvIdSource>& blacklistedSvIds)
{
    // Clear the existing config
    memset(&mGnssSvIdConfig, 0, sizeof(GnssSvIdConfig));

    // Convert the sv id lists to masks
    bool convertSuccess = convertToGnssSvIdConfig(blacklistedSvIds, mGnssSvIdConfig);

    // Now send to Modem if conversion successful
    if (convertSuccess) {
        gnssSvIdConfigUpdate();
    } else {
        LOC_LOGe("convertToGnssSvIdConfig failed");
    }
}

void
GnssAdapter::gnssSvIdConfigUpdate()
{
    LOC_LOGd("blacklist bds 0x%" PRIx64 ", glo 0x%" PRIx64
            ", qzss 0x%" PRIx64 ", gal 0x%" PRIx64 ", sbas 0x%" PRIx64 ", navic 0x%" PRIx64,
            mGnssSvIdConfig.bdsBlacklistSvMask, mGnssSvIdConfig.gloBlacklistSvMask,
            mGnssSvIdConfig.qzssBlacklistSvMask, mGnssSvIdConfig.galBlacklistSvMask,
            mGnssSvIdConfig.sbasBlacklistSvMask, mGnssSvIdConfig.navicBlacklistSvMask);
    // Now set required blacklisted SVs
    mLocApi->setBlacklistSv(mGnssSvIdConfig);
}

LocationError
GnssAdapter::gnssSvIdConfigUpdateSync(const std::vector<GnssSvIdSource>& blacklistedSvIds)
{
    // Clear the existing config
    memset(&mGnssSvIdConfig, 0, sizeof(GnssSvIdConfig));

    // Convert the sv id lists to masks
    convertToGnssSvIdConfig(blacklistedSvIds, mGnssSvIdConfig);

    // Now send to Modem
    return gnssSvIdConfigUpdateSync();
}

LocationError
GnssAdapter::gnssSvIdConfigUpdateSync()
{
    LOC_LOGd("blacklist bds 0x%" PRIx64 ", glo 0x%" PRIx64
            ", qzss 0x%" PRIx64 ", gal 0x%" PRIx64 ", sbas 0x%" PRIx64 ", navic 0x%" PRIx64,
            mGnssSvIdConfig.bdsBlacklistSvMask, mGnssSvIdConfig.gloBlacklistSvMask,
            mGnssSvIdConfig.qzssBlacklistSvMask, mGnssSvIdConfig.galBlacklistSvMask,
            mGnssSvIdConfig.sbasBlacklistSvMask, mGnssSvIdConfig.navicBlacklistSvMask);

    // Now set required blacklisted SVs
    return mLocApi->setBlacklistSvSync(mGnssSvIdConfig);
}

void
GnssAdapter::gnssSecondaryBandConfigUpdate(LocApiResponse* locApiResponse)
{
    LOC_LOGd("secondary band config, size %d, enabled constellation 0x%" PRIx64 ","
             "disabled constellation 0x%" PRIx64 "", mGnssSeconaryBandConfig.size,
             mGnssSeconaryBandConfig.enabledSvTypesMask,
             mGnssSeconaryBandConfig.blacklistedSvTypesMask);
    if (mGnssSeconaryBandConfig.size == sizeof(mGnssSeconaryBandConfig)) {
        // Now set required secondary band config
        mLocApi->configConstellationMultiBand(mGnssSeconaryBandConfig, locApiResponse);
    }
}

uint32_t*
GnssAdapter::gnssGetConfigCommand(GnssConfigFlagsMask configMask) {

    // count the number of bits set
    GnssConfigFlagsMask flagsCopy = configMask;
    size_t count = 0;
    while (flagsCopy > 0) {
        if (flagsCopy & 1) {
            count++;
        }
        flagsCopy >>= 1;
    }
    std::string idsString = "[";
    uint32_t* ids = NULL;
    if (count > 0) {
        ids = new uint32_t[count];
        if (nullptr == ids) {
            LOC_LOGe("new allocation failed, fatal error.");
            return nullptr;
        }
        for (size_t i=0; i < count; ++i) {
            ids[i] = generateSessionId();
            IF_LOC_LOGD {
                idsString += std::to_string(ids[i]) + " ";
            }
        }
    }
    idsString += "]";

    LOC_LOGd("ids %s flags 0x%X", idsString.c_str(), configMask);

    struct MsgGnssGetConfig : public LocMsg {
        GnssAdapter& mAdapter;
        LocApiBase& mApi;
        GnssConfigFlagsMask mConfigMask;
        uint32_t* mIds;
        size_t mCount;
        inline MsgGnssGetConfig(GnssAdapter& adapter,
                                LocApiBase& api,
                                GnssConfigFlagsMask configMask,
                                uint32_t* ids,
                                size_t count) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api),
            mConfigMask(configMask),
            mCount(count),
            mIds(nullptr) {
                if (mCount > 0) {
                    mIds = new uint32_t[count];
                    if (mIds) {
                        for (uint32_t index = 0; index < count; index++) {
                            mIds[index] = ids[index];
                        }
                    } else {
                        LOC_LOGe("memory allocation for mIds failed");
                    }
                }
            }

        inline virtual ~MsgGnssGetConfig()
        {
            delete[] mIds;
        }
        inline virtual void proc() const {

            LocationError* errs = new LocationError[mCount];
            LocationError err = LOCATION_ERROR_SUCCESS;
            uint32_t index = 0;

            if (nullptr == errs) {
                LOC_LOGE("%s] new allocation failed, fatal error.", __func__);
                return;
            }

            if (mConfigMask & GNSS_CONFIG_FLAGS_GPS_LOCK_VALID_BIT) {
                if (index < mCount) {
                    errs[index++] = LOCATION_ERROR_NOT_SUPPORTED;
                }
            }
            if (mConfigMask & GNSS_CONFIG_FLAGS_SUPL_VERSION_VALID_BIT) {
                if (index < mCount) {
                    errs[index++] = LOCATION_ERROR_NOT_SUPPORTED;
                }
            }
            if (mConfigMask & GNSS_CONFIG_FLAGS_SET_ASSISTANCE_DATA_VALID_BIT) {
                if (index < mCount) {
                    errs[index++] = LOCATION_ERROR_NOT_SUPPORTED;
                }
            }
            if (mConfigMask & GNSS_CONFIG_FLAGS_LPP_PROFILE_VALID_BIT) {
                if (index < mCount) {
                    errs[index++] = LOCATION_ERROR_NOT_SUPPORTED;
                }
            }
            if (mConfigMask & GNSS_CONFIG_FLAGS_LPPE_CONTROL_PLANE_VALID_BIT) {
                if (index < mCount) {
                    errs[index++] = LOCATION_ERROR_NOT_SUPPORTED;
                }
            }
            if (mConfigMask & GNSS_CONFIG_FLAGS_LPPE_USER_PLANE_VALID_BIT) {
                if (index < mCount) {
                    errs[index++] = LOCATION_ERROR_NOT_SUPPORTED;
                }
            }
            if (mConfigMask & GNSS_CONFIG_FLAGS_AGLONASS_POSITION_PROTOCOL_VALID_BIT) {
                if (index < mCount) {
                    errs[index++] = LOCATION_ERROR_NOT_SUPPORTED;
                }
            }
            if (mConfigMask & GNSS_CONFIG_FLAGS_EM_PDN_FOR_EM_SUPL_VALID_BIT) {
                if (index < mCount) {
                    errs[index++] = LOCATION_ERROR_NOT_SUPPORTED;
                }
            }
            if (mConfigMask & GNSS_CONFIG_FLAGS_SUPL_EM_SERVICES_BIT) {
                if (index < mCount) {
                    errs[index++] = LOCATION_ERROR_NOT_SUPPORTED;
                }
            }
            if (mConfigMask & GNSS_CONFIG_FLAGS_SUPL_MODE_BIT) {
                err = LOCATION_ERROR_NOT_SUPPORTED;
                if (index < mCount) {
                    errs[index++] = LOCATION_ERROR_NOT_SUPPORTED;
                }
            }
            if (mConfigMask & GNSS_CONFIG_FLAGS_BLACKLISTED_SV_IDS_BIT) {
                // Check if feature is supported
                if (!ContextBase::isFeatureSupported(
                        LOC_SUPPORTED_FEATURE_CONSTELLATION_ENABLEMENT_V02)) {
                    LOC_LOGe("Feature not supported.");
                    err = LOCATION_ERROR_NOT_SUPPORTED;
                } else {
                    // Send request to Modem to fetch the config
                    mApi.getBlacklistSv();
                    err = LOCATION_ERROR_SUCCESS;
                }
                if (index < mCount) {
                    errs[index++] = err;
                }
            }
            if (mConfigMask & GNSS_CONFIG_FLAGS_ROBUST_LOCATION_BIT) {
                uint32_t sessionId = *(mIds+index);
                LocApiResponse* locApiResponse =
                        new LocApiResponse(*mAdapter.getContext(),
                                           [this, sessionId] (LocationError err) {
                                           mAdapter.reportResponse(err, sessionId);});
                if (!locApiResponse) {
                    LOC_LOGe("memory alloc failed");
                    mAdapter.reportResponse(LOCATION_ERROR_GENERAL_FAILURE, sessionId);
                } else {
                   mApi.getRobustLocationConfig(sessionId, locApiResponse);
                }
            }

            if (mConfigMask & GNSS_CONFIG_FLAGS_MIN_GPS_WEEK_BIT) {
                uint32_t sessionId = *(mIds+index);
                LocApiResponse* locApiResponse =
                        new LocApiResponse(*mAdapter.getContext(),
                                           [this, sessionId] (LocationError err) {
                                           mAdapter.reportResponse(err, sessionId);});
                if (!locApiResponse) {
                    LOC_LOGe("memory alloc failed");
                    mAdapter.reportResponse(LOCATION_ERROR_GENERAL_FAILURE, sessionId);
                } else {
                   mApi.getMinGpsWeek(sessionId, locApiResponse);
                }
            }

            if (mConfigMask & GNSS_CONFIG_FLAGS_MIN_SV_ELEVATION_BIT) {
                uint32_t sessionId = *(mIds+index);
                LocApiResponse* locApiResponse =
                        new LocApiResponse(*mAdapter.getContext(),
                                           [this, sessionId] (LocationError err) {
                                           mAdapter.reportResponse(err, sessionId);});
                if (!locApiResponse) {
                    LOC_LOGe("memory alloc failed");
                    mAdapter.reportResponse(LOCATION_ERROR_GENERAL_FAILURE, sessionId);
                } else {
                    mApi.getParameter(sessionId, GNSS_CONFIG_FLAGS_MIN_SV_ELEVATION_BIT,
                                      locApiResponse);
                }
            }

            mAdapter.reportResponse(index, errs, mIds);
            delete[] errs;

        }
    };

    if (NULL != ids) {
        sendMsg(new MsgGnssGetConfig(*this, *mLocApi, configMask, ids, count));
    } else {
        LOC_LOGe("No GNSS config items to Get");
    }

    return ids;
}

bool
GnssAdapter::convertToGnssSvIdConfig(
        const std::vector<GnssSvIdSource>& blacklistedSvIds, GnssSvIdConfig& config)
{
    bool retVal = false;
    config.size = sizeof(GnssSvIdConfig);

    // Empty vector => Clear any previous blacklisted SVs
    if (0 == blacklistedSvIds.size()) {
        config.gloBlacklistSvMask = 0;
        config.bdsBlacklistSvMask = 0;
        config.qzssBlacklistSvMask = 0;
        config.galBlacklistSvMask = 0;
        config.sbasBlacklistSvMask = 0;
        config.navicBlacklistSvMask = 0;
        retVal = true;
    } else {
        // Parse the vector and convert SV IDs to mask values
        for (GnssSvIdSource source : blacklistedSvIds) {
            uint64_t* svMaskPtr = NULL;
            GnssSvId initialSvId = 0;
            uint16_t svIndexOffset = 0;
            switch(source.constellation) {
            case GNSS_SV_TYPE_GLONASS:
                svMaskPtr = &config.gloBlacklistSvMask;
                initialSvId = GNSS_SV_CONFIG_GLO_INITIAL_SV_ID;
                break;
            case GNSS_SV_TYPE_BEIDOU:
                svMaskPtr = &config.bdsBlacklistSvMask;
                initialSvId = GNSS_SV_CONFIG_BDS_INITIAL_SV_ID;
                break;
            case GNSS_SV_TYPE_QZSS:
                svMaskPtr = &config.qzssBlacklistSvMask;
                initialSvId = GNSS_SV_CONFIG_QZSS_INITIAL_SV_ID;
                break;
            case GNSS_SV_TYPE_GALILEO:
                svMaskPtr = &config.galBlacklistSvMask;
                initialSvId = GNSS_SV_CONFIG_GAL_INITIAL_SV_ID;
                break;
            case GNSS_SV_TYPE_SBAS:
                // SBAS does not support enable/disable whole constellation
                // so do not set up svTypeMask for SBAS
                svMaskPtr = &config.sbasBlacklistSvMask;
                // SBAS currently has two ranges, [120, 158] and [183, 191]
                if (0 == source.svId) {
                    LOC_LOGd("blacklist all SBAS SV");
                } else if (source.svId >= GNSS_SV_CONFIG_SBAS_INITIAL2_SV_ID) {
                    // handle SV id in range [183, 191]
                    initialSvId = GNSS_SV_CONFIG_SBAS_INITIAL2_SV_ID;
                    svIndexOffset = GNSS_SV_CONFIG_SBAS_INITIAL_SV_LENGTH;
                } else if ((source.svId >= GNSS_SV_CONFIG_SBAS_INITIAL_SV_ID) &&
                           (source.svId < (GNSS_SV_CONFIG_SBAS_INITIAL_SV_ID +
                                           GNSS_SV_CONFIG_SBAS_INITIAL_SV_LENGTH))){
                        // handle SV id in range of [120, 158]
                        initialSvId = GNSS_SV_CONFIG_SBAS_INITIAL_SV_ID;
                    } else {
                        LOC_LOGe("invalid SBAS sv id %d", source.svId);
                        svMaskPtr = nullptr;
                    }
                    break;
            case GNSS_SV_TYPE_NAVIC:
                 svMaskPtr = &config.navicBlacklistSvMask;
                 initialSvId = GNSS_SV_CONFIG_NAVIC_INITIAL_SV_ID;
                break;
            default:
                break;
            }

            if (NULL == svMaskPtr) {
                LOC_LOGe("Invalid constellation %d", source.constellation);
            } else {
                // SV ID 0 = All SV IDs
                if (0 == source.svId) {
                    *svMaskPtr = GNSS_SV_CONFIG_ALL_BITS_ENABLED_MASK;
                } else if (source.svId < initialSvId || source.svId >= initialSvId + 64) {
                    LOC_LOGe("Invalid sv id %d for sv type %d",
                            source.svId, source.constellation);
                } else {
                    uint32_t shiftCnt = source.svId + svIndexOffset - initialSvId;
                    *svMaskPtr |= (1ULL << shiftCnt);
                }
            }
        }

        // Return true if any one source is valid
        if (0 != config.gloBlacklistSvMask ||
                0 != config.bdsBlacklistSvMask ||
                0 != config.galBlacklistSvMask ||
                0 != config.qzssBlacklistSvMask ||
                0 != config.sbasBlacklistSvMask ||
                0 != config.navicBlacklistSvMask) {
            retVal = true;
        }
    }

    LOC_LOGd("blacklist bds 0x%" PRIx64 ", glo 0x%" PRIx64
            ", qzss 0x%" PRIx64 ", gal 0x%" PRIx64 ", sbas 0x%" PRIx64 ", navic 0x%" PRIx64,
             config.bdsBlacklistSvMask, config.gloBlacklistSvMask,
             config.qzssBlacklistSvMask, config.galBlacklistSvMask,
            config.sbasBlacklistSvMask, config.navicBlacklistSvMask);

    return retVal;
}

void GnssAdapter::convertFromGnssSvIdConfig(
        const GnssSvIdConfig& svConfig, std::vector<GnssSvIdSource>& blacklistedSvIds)
{
    // Convert blacklisted SV mask values to vectors
    if (svConfig.bdsBlacklistSvMask) {
        convertGnssSvIdMaskToList(
                svConfig.bdsBlacklistSvMask, blacklistedSvIds,
                GNSS_SV_CONFIG_BDS_INITIAL_SV_ID, GNSS_SV_TYPE_BEIDOU);
    }
    if (svConfig.galBlacklistSvMask) {
        convertGnssSvIdMaskToList(
                svConfig.galBlacklistSvMask, blacklistedSvIds,
                GNSS_SV_CONFIG_GAL_INITIAL_SV_ID, GNSS_SV_TYPE_GALILEO);
    }
    if (svConfig.gloBlacklistSvMask) {
        convertGnssSvIdMaskToList(
                svConfig.gloBlacklistSvMask, blacklistedSvIds,
                GNSS_SV_CONFIG_GLO_INITIAL_SV_ID, GNSS_SV_TYPE_GLONASS);
    }
    if (svConfig.qzssBlacklistSvMask) {
        convertGnssSvIdMaskToList(
                svConfig.qzssBlacklistSvMask, blacklistedSvIds,
                GNSS_SV_CONFIG_QZSS_INITIAL_SV_ID, GNSS_SV_TYPE_QZSS);
    }
    if (svConfig.sbasBlacklistSvMask) {
        // SBAS - SV 120 to 158, maps to 0 to 38
        //        SV 183 to 191, maps to 39 to 47
        uint64_t sbasBlacklistSvMask = svConfig.sbasBlacklistSvMask;
        // operate on 120 and 158 first
        sbasBlacklistSvMask <<= (64 - GNSS_SV_CONFIG_SBAS_INITIAL_SV_LENGTH);
        sbasBlacklistSvMask >>= (64 - GNSS_SV_CONFIG_SBAS_INITIAL_SV_LENGTH);
        convertGnssSvIdMaskToList(
                sbasBlacklistSvMask, blacklistedSvIds,
                GNSS_SV_CONFIG_SBAS_INITIAL_SV_ID, GNSS_SV_TYPE_SBAS);
        // operate on the second range
        sbasBlacklistSvMask = svConfig.sbasBlacklistSvMask;
        sbasBlacklistSvMask >>= GNSS_SV_CONFIG_SBAS_INITIAL_SV_LENGTH;
        convertGnssSvIdMaskToList(
                sbasBlacklistSvMask, blacklistedSvIds,
                GNSS_SV_CONFIG_SBAS_INITIAL2_SV_ID, GNSS_SV_TYPE_SBAS);
    }
    if (svConfig.navicBlacklistSvMask) {
        convertGnssSvIdMaskToList(
                svConfig.navicBlacklistSvMask, blacklistedSvIds,
                GNSS_SV_CONFIG_NAVIC_INITIAL_SV_ID, GNSS_SV_TYPE_NAVIC);
    }
}

void GnssAdapter::convertGnssSvIdMaskToList(
        uint64_t svIdMask, std::vector<GnssSvIdSource>& svIds,
        GnssSvId initialSvId, GnssSvType svType)
{
    GnssSvIdSource source = {};
    source.size = sizeof(GnssSvIdSource);
    source.constellation = svType;

    // SV ID 0 => All SV IDs in mask
    if (GNSS_SV_CONFIG_ALL_BITS_ENABLED_MASK == svIdMask) {
        LOC_LOGd("blacklist all SVs in constellation %d", source.constellation);
        source.svId = 0;
        svIds.push_back(source);
        return;
    }

    // Convert each bit in svIdMask to vector entry
    uint32_t bitNumber = 0;
    while (svIdMask > 0) {
        if (svIdMask & 0x1) {
            source.svId = bitNumber + initialSvId;
            // SBAS has two ranges:
            // SBAS - SV 120 to 158, maps to 0 to 38
            //        SV 183 to 191, maps to 39 to 47
            // #define GNSS_SV_CONFIG_SBAS_INITIAL_SV_ID     120
            // #define GNSS_SV_CONFIG_SBAS_INITIAL_SV_LENGTH 39
            // #define GNSS_SV_CONFIG_SBAS_INITIAL2_SV_ID    183
            if (svType == GNSS_SV_TYPE_SBAS) {
                if (bitNumber >= GNSS_SV_CONFIG_SBAS_INITIAL_SV_LENGTH) {
                    source.svId = bitNumber - GNSS_SV_CONFIG_SBAS_INITIAL_SV_LENGTH +
                                  GNSS_SV_CONFIG_SBAS_INITIAL2_SV_ID;
                }
            }
            svIds.push_back(source);
        }
        bitNumber++;
        svIdMask >>= 1;
    }
}

void GnssAdapter::reportGnssSvIdConfigEvent(const GnssSvIdConfig& config)
{
    struct MsgReportGnssSvIdConfig : public LocMsg {
        GnssAdapter& mAdapter;
        const GnssSvIdConfig mConfig;
        inline MsgReportGnssSvIdConfig(GnssAdapter& adapter,
                                 const GnssSvIdConfig& config) :
            LocMsg(),
            mAdapter(adapter),
            mConfig(config) {}
        inline virtual void proc() const {
            mAdapter.reportGnssSvIdConfig(mConfig);
        }
    };

    sendMsg(new MsgReportGnssSvIdConfig(*this, config));
}

void GnssAdapter::reportGnssSvIdConfig(const GnssSvIdConfig& svIdConfig)
{
    GnssConfig config = {};
    config.size = sizeof(GnssConfig);

    // Invoke control clients config callback
    if (nullptr != mControlCallbacks.gnssConfigCb &&
            svIdConfig.size == sizeof(GnssSvIdConfig)) {

        convertFromGnssSvIdConfig(svIdConfig, config.blacklistedSvIds);
        if (config.blacklistedSvIds.size() > 0) {
            config.flags |= GNSS_CONFIG_FLAGS_BLACKLISTED_SV_IDS_BIT;
        }
        LOC_LOGd("blacklist bds 0x%" PRIx64 ", glo 0x%" PRIx64 ", "
                 "qzss 0x%" PRIx64 ", gal 0x%" PRIx64 ", sbas 0x%" PRIx64 ", navic 0x%" PRIx64,
                 svIdConfig.bdsBlacklistSvMask, svIdConfig.gloBlacklistSvMask,
                 svIdConfig.qzssBlacklistSvMask, svIdConfig.galBlacklistSvMask,
                 svIdConfig.sbasBlacklistSvMask,  svIdConfig.navicBlacklistSvMask);
        // use 0 session id to indicate that receiver does not yet care about session id
        mControlCallbacks.gnssConfigCb(0, config);
    } else {
        LOC_LOGe("Failed to report, size %d", (uint32_t)config.size);
    }
}

void
GnssAdapter::gnssUpdateSvTypeConfigCommand(GnssSvTypeConfig config)
{
    struct MsgGnssUpdateSvTypeConfig : public LocMsg {
        GnssAdapter* mAdapter;
        LocApiBase* mApi;
        GnssSvTypeConfig mConfig;
        inline MsgGnssUpdateSvTypeConfig(
                GnssAdapter* adapter,
                LocApiBase* api,
                GnssSvTypeConfig& config) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api),
            mConfig(config) {}
        inline virtual void proc() const {

            // Check if feature is supported
            if (!ContextBase::isFeatureSupported(
                    LOC_SUPPORTED_FEATURE_CONSTELLATION_ENABLEMENT_V02)) {
                LOC_LOGe("Feature not supported.");
            } else {
                // Send update request to modem
                mAdapter->gnssSvTypeConfigUpdate(mConfig);
            }
        }
    };

    sendMsg(new MsgGnssUpdateSvTypeConfig(this, mLocApi, config));
}

void
GnssAdapter::gnssSvTypeConfigUpdate(const GnssSvTypeConfig& config)
{
    // Gather bits removed from enabled mask
    GnssSvTypesMask enabledRemoved = mGnssSvTypeConfig.enabledSvTypesMask &
            (mGnssSvTypeConfig.enabledSvTypesMask ^ config.enabledSvTypesMask);
    // Send reset if any constellation is removed from the enabled list
    bool sendReset = (enabledRemoved != 0);
    // Save new config and update
    gnssSetSvTypeConfig(config);
    gnssSvTypeConfigUpdate(sendReset);
}

void
GnssAdapter::gnssSvTypeConfigUpdate(bool sendReset)
{
    LOC_LOGd("constellations blacklisted 0x%" PRIx64 ", enabled 0x%" PRIx64 ", sendReset %d",
             mGnssSvTypeConfig.blacklistedSvTypesMask, mGnssSvTypeConfig.enabledSvTypesMask,
             sendReset);

    LOC_LOGd("blacklist bds 0x%" PRIx64 ", glo 0x%" PRIx64
            ", qzss 0x%" PRIx64 ", gal 0x%" PRIx64 ", sbas 0x%" PRIx64 ", Navic 0x%" PRIx64,
            mGnssSvIdConfig.bdsBlacklistSvMask, mGnssSvIdConfig.gloBlacklistSvMask,
            mGnssSvIdConfig.qzssBlacklistSvMask, mGnssSvIdConfig.galBlacklistSvMask,
            mGnssSvIdConfig.sbasBlacklistSvMask, mGnssSvIdConfig.navicBlacklistSvMask);

    if (mGnssSvTypeConfig.size == sizeof(mGnssSvTypeConfig)) {

        if (sendReset) {
            mLocApi->resetConstellationControl();
        }

        GnssSvIdConfig blacklistConfig = {};
        // Revert to previously blacklisted SVs for each enabled constellation
        blacklistConfig = mGnssSvIdConfig;
        // Blacklist all SVs for each disabled constellation
        if (mGnssSvTypeConfig.blacklistedSvTypesMask) {
            if (mGnssSvTypeConfig.blacklistedSvTypesMask & GNSS_SV_TYPES_MASK_GLO_BIT) {
                blacklistConfig.gloBlacklistSvMask = GNSS_SV_CONFIG_ALL_BITS_ENABLED_MASK;
            }
            if (mGnssSvTypeConfig.blacklistedSvTypesMask & GNSS_SV_TYPES_MASK_BDS_BIT) {
                blacklistConfig.bdsBlacklistSvMask = GNSS_SV_CONFIG_ALL_BITS_ENABLED_MASK;
            }
            if (mGnssSvTypeConfig.blacklistedSvTypesMask & GNSS_SV_TYPES_MASK_QZSS_BIT) {
                blacklistConfig.qzssBlacklistSvMask = GNSS_SV_CONFIG_ALL_BITS_ENABLED_MASK;
            }
            if (mGnssSvTypeConfig.blacklistedSvTypesMask & GNSS_SV_TYPES_MASK_GAL_BIT) {
                blacklistConfig.galBlacklistSvMask = GNSS_SV_CONFIG_ALL_BITS_ENABLED_MASK;
            }
            if (mGnssSvTypeConfig.blacklistedSvTypesMask & GNSS_SV_TYPES_MASK_NAVIC_BIT) {
                blacklistConfig.navicBlacklistSvMask = GNSS_SV_CONFIG_ALL_BITS_ENABLED_MASK;
            }
        }

        // Send blacklist info
        mLocApi->setBlacklistSv(blacklistConfig);

        // Send only enabled constellation config
        if (mGnssSvTypeConfig.enabledSvTypesMask) {
            GnssSvTypeConfig svTypeConfig = {sizeof(GnssSvTypeConfig), 0, 0};
            svTypeConfig.enabledSvTypesMask = mGnssSvTypeConfig.enabledSvTypesMask;
            mLocApi->setConstellationControl(svTypeConfig);
        }
    } else {
        LOC_LOGE("Invalid GnssSvTypeConfig size");
    }
}

void
GnssAdapter::gnssGetSvTypeConfigCommand(GnssSvTypeConfigCallback callback)
{
    struct MsgGnssGetSvTypeConfig : public LocMsg {
        GnssAdapter* mAdapter;
        LocApiBase* mApi;
        GnssSvTypeConfigCallback mCallback;
        inline MsgGnssGetSvTypeConfig(
                GnssAdapter* adapter,
                LocApiBase* api,
                GnssSvTypeConfigCallback callback) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api),
            mCallback(callback) {}
        inline virtual void proc() const {

            if (!ContextBase::isFeatureSupported(
                    LOC_SUPPORTED_FEATURE_CONSTELLATION_ENABLEMENT_V02)) {
                LOC_LOGe("Feature not supported.");
            } else {
                // Save the callback
                mAdapter->gnssSetSvTypeConfigCallback(mCallback);
                // Send GET request to modem
                mApi->getConstellationControl();
            }
        }
    };

    sendMsg(new MsgGnssGetSvTypeConfig(this, mLocApi, callback));
}

void
GnssAdapter::gnssResetSvTypeConfigCommand()
{
    struct MsgGnssResetSvTypeConfig : public LocMsg {
        GnssAdapter* mAdapter;
        LocApiBase* mApi;
        inline MsgGnssResetSvTypeConfig(
                GnssAdapter* adapter,
                LocApiBase* api) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api) {}
        inline virtual void proc() const {

            if (!ContextBase::isFeatureSupported(
                    LOC_SUPPORTED_FEATURE_CONSTELLATION_ENABLEMENT_V02)) {
                LOC_LOGe("Feature not supported.");
            } else {
                // Reset constellation config
                mAdapter->gnssSetSvTypeConfig({sizeof(GnssSvTypeConfig), 0, 0});
                // Re-enforce SV blacklist config
                mAdapter->gnssSvIdConfigUpdate();
                // Send reset request to modem
                mApi->resetConstellationControl();
            }
        }
    };

    sendMsg(new MsgGnssResetSvTypeConfig(this, mLocApi));
}

void GnssAdapter::reportGnssSvTypeConfigEvent(const GnssSvTypeConfig& config)
{
    struct MsgReportGnssSvTypeConfig : public LocMsg {
        GnssAdapter& mAdapter;
        const GnssSvTypeConfig mConfig;
        inline MsgReportGnssSvTypeConfig(GnssAdapter& adapter,
                                 const GnssSvTypeConfig& config) :
            LocMsg(),
            mAdapter(adapter),
            mConfig(config) {}
        inline virtual void proc() const {
            mAdapter.reportGnssSvTypeConfig(mConfig);
        }
    };

    sendMsg(new MsgReportGnssSvTypeConfig(*this, config));
}

void GnssAdapter::reportGnssSvTypeConfig(const GnssSvTypeConfig& config)
{
    // Invoke Get SV Type Callback
    if (NULL != mGnssSvTypeConfigCb &&
            config.size == sizeof(GnssSvTypeConfig)) {
        LOC_LOGd("constellations blacklisted 0x%" PRIx64 ", enabled 0x%" PRIx64,
                 config.blacklistedSvTypesMask, config.enabledSvTypesMask);
        mGnssSvTypeConfigCb(config);
    } else {
        LOC_LOGe("Failed to report, size %d", (uint32_t)config.size);
    }
}

void GnssAdapter::deleteAidingData(const GnssAidingData &data, uint32_t sessionId) {
    mLocApi->deleteAidingData(data, new LocApiResponse(*getContext(),
            [this, sessionId] (LocationError err) {
                reportResponse(err, sessionId);
            }));
}

uint32_t
GnssAdapter::gnssDeleteAidingDataCommand(GnssAidingData& data)
{
    uint32_t sessionId = generateSessionId();
    LOC_LOGD("%s]: id %u", __func__, sessionId);

    struct MsgDeleteAidingData : public LocMsg {
        GnssAdapter& mAdapter;
        uint32_t mSessionId;
        GnssAidingData mData;
        inline MsgDeleteAidingData(GnssAdapter& adapter,
                                   uint32_t sessionId,
                                   GnssAidingData& data) :
            LocMsg(),
            mAdapter(adapter),
            mSessionId(sessionId),
            mData(data) {}
        inline virtual void proc() const {
            if ((mData.posEngineMask & STANDARD_POSITIONING_ENGINE) != 0) {
                mAdapter.deleteAidingData(mData, mSessionId);
                SystemStatus* s = mAdapter.getSystemStatus();
                if ((nullptr != s) && (mData.deleteAll)) {
                    s->setDefaultGnssEngineStates();
                }
            }

            bool retVal = mAdapter.mEngHubProxy->gnssDeleteAidingData(mData);
            // When SPE engine is invoked, responseCb will be invoked
            // from QMI Loc API call.
            // When SPE engine is not invoked, we also need to deliver responseCb
            if ((mData.posEngineMask & STANDARD_POSITIONING_ENGINE) == 0) {
                LocationError err = LOCATION_ERROR_NOT_SUPPORTED;
                if (retVal == true) {
                    err = LOCATION_ERROR_SUCCESS;
                }
                mAdapter.reportResponse(err, mSessionId);
            }
        }
    };

    sendMsg(new MsgDeleteAidingData(*this, sessionId, data));
    return sessionId;
}

void
GnssAdapter::gnssUpdateXtraThrottleCommand(const bool enabled)
{
    LOC_LOGD("%s] enabled:%d", __func__, enabled);

    struct UpdateXtraThrottleMsg : public LocMsg {
        GnssAdapter& mAdapter;
        const bool mEnabled;
        inline UpdateXtraThrottleMsg(GnssAdapter& adapter, const bool enabled) :
            LocMsg(),
            mAdapter(adapter),
            mEnabled(enabled) {}
        inline virtual void proc() const {
                mAdapter.mXtraObserver.updateXtraThrottle(mEnabled);
        }
    };

    sendMsg(new UpdateXtraThrottleMsg(*this, enabled));
}

void
GnssAdapter::injectLocationCommand(double latitude, double longitude, float accuracy)
{
    LOC_LOGD("%s]: latitude %8.4f longitude %8.4f accuracy %8.4f",
             __func__, latitude, longitude, accuracy);

    struct MsgInjectLocation : public LocMsg {
        LocApiBase& mApi;
        ContextBase& mContext;
        BlockCPIInfo& mBlockCPI;
        double mLatitude;
        double mLongitude;
        float mAccuracy;
        inline MsgInjectLocation(LocApiBase& api,
                                 ContextBase& context,
                                 BlockCPIInfo& blockCPIInfo,
                                 double latitude,
                                 double longitude,
                                 float accuracy) :
            LocMsg(),
            mApi(api),
            mContext(context),
            mBlockCPI(blockCPIInfo),
            mLatitude(latitude),
            mLongitude(longitude),
            mAccuracy(accuracy) {}
        inline virtual void proc() const {
            if ((uptimeMillis() <= mBlockCPI.blockedTillTsMs) &&
                (fabs(mLatitude-mBlockCPI.latitude) <= mBlockCPI.latLonDiffThreshold) &&
                (fabs(mLongitude-mBlockCPI.longitude) <= mBlockCPI.latLonDiffThreshold)) {

                LOC_LOGD("%s]: positon injeciton blocked: lat: %f, lon: %f, accuracy: %f",
                         __func__, mLatitude, mLongitude, mAccuracy);

            } else {
                mApi.injectPosition(mLatitude, mLongitude, mAccuracy);
            }
        }
    };

    sendMsg(new MsgInjectLocation(*mLocApi, *mContext, mBlockCPIInfo,
                                  latitude, longitude, accuracy));
}

void
GnssAdapter::injectLocationExtCommand(const GnssLocationInfoNotification &locationInfo)
{
    LOC_LOGd("latitude %8.4f longitude %8.4f accuracy %8.4f, tech mask 0x%x",
             locationInfo.location.latitude, locationInfo.location.longitude,
             locationInfo.location.accuracy, locationInfo.location.techMask);

    struct MsgInjectLocationExt : public LocMsg {
        LocApiBase& mApi;
        ContextBase& mContext;
        GnssLocationInfoNotification mLocationInfo;
        inline MsgInjectLocationExt(LocApiBase& api,
                                    ContextBase& context,
                                    GnssLocationInfoNotification locationInfo) :
            LocMsg(),
            mApi(api),
            mContext(context),
            mLocationInfo(locationInfo) {}
        inline virtual void proc() const {
            // false to indicate for none-ODCPI
            mApi.injectPosition(mLocationInfo, false);
        }
    };

    sendMsg(new MsgInjectLocationExt(*mLocApi, *mContext, locationInfo));
}

void
GnssAdapter::injectTimeCommand(int64_t time, int64_t timeReference, int32_t uncertainty)
{
    LOC_LOGD("%s]: time %lld timeReference %lld uncertainty %d",
             __func__, (long long)time, (long long)timeReference, uncertainty);

    struct MsgInjectTime : public LocMsg {
        LocApiBase& mApi;
        ContextBase& mContext;
        int64_t mTime;
        int64_t mTimeReference;
        int32_t mUncertainty;
        inline MsgInjectTime(LocApiBase& api,
                             ContextBase& context,
                             int64_t time,
                             int64_t timeReference,
                             int32_t uncertainty) :
            LocMsg(),
            mApi(api),
            mContext(context),
            mTime(time),
            mTimeReference(timeReference),
            mUncertainty(uncertainty) {}
        inline virtual void proc() const {
            mApi.setTime(mTime, mTimeReference, mUncertainty);
        }
    };

    sendMsg(new MsgInjectTime(*mLocApi, *mContext, time, timeReference, uncertainty));
}

// This command is to called to block the position to be injected to the modem.
// This can happen for network position that comes from modem.
void
GnssAdapter::blockCPICommand(double latitude, double longitude,
                             float accuracy, int blockDurationMsec,
                             double latLonDiffThreshold)
{
    struct MsgBlockCPI : public LocMsg {
        BlockCPIInfo& mDstCPIInfo;
        BlockCPIInfo mSrcCPIInfo;

        inline MsgBlockCPI(BlockCPIInfo& dstCPIInfo,
                           BlockCPIInfo& srcCPIInfo) :
            mDstCPIInfo(dstCPIInfo),
            mSrcCPIInfo(srcCPIInfo) {}
        inline virtual void proc() const {
            // in the same hal thread, save the cpi to be blocked
            // the global variable
            mDstCPIInfo = mSrcCPIInfo;
        }
    };

    // construct the new block CPI info and queue on the same thread
    // for processing
    BlockCPIInfo blockCPIInfo;
    blockCPIInfo.latitude = latitude;
    blockCPIInfo.longitude = longitude;
    blockCPIInfo.accuracy = accuracy;
    blockCPIInfo.blockedTillTsMs = uptimeMillis() + blockDurationMsec;
    blockCPIInfo.latLonDiffThreshold = latLonDiffThreshold;

    LOC_LOGD("%s]: block CPI lat: %f, lon: %f ", __func__, latitude, longitude);
    // send a message to record down the coarse position
    // to be blocked from injection in the master copy (mBlockCPIInfo)
    sendMsg(new MsgBlockCPI(mBlockCPIInfo, blockCPIInfo));
}

void
GnssAdapter::updatePowerState(PowerStateType powerState) {
    if (POWER_STATE_UNKNOWN != powerState) {
        mPowerState = powerState;
        mLocApi->updatePowerState(mPowerState);
    }
}

void
GnssAdapter::updatePowerStateCommand(PowerStateType powerState) {
    LOC_LOGd("power event %d", powerState);

    struct MsgUpdatePowerState : public LocMsg {
        GnssAdapter& mAdapter;
        PowerStateType mPowerState;

        inline MsgUpdatePowerState(GnssAdapter& adapter,
                                   PowerStateType powerState) :
            LocMsg(),
            mAdapter(adapter),
            mPowerState(powerState) {}
        inline virtual void proc() const {
            mAdapter.updatePowerState(mPowerState);
        }
    };

    sendMsg(new MsgUpdatePowerState(*this, powerState));
}

void
GnssAdapter::addClientCommand(LocationAPI* client, const LocationCallbacks& callbacks)
{
    LOC_LOGD("%s]: client %p", __func__, client);

    struct MsgAddClient : public LocMsg {
        GnssAdapter& mAdapter;
        LocationAPI* mClient;
        const LocationCallbacks mCallbacks;
        inline MsgAddClient(GnssAdapter& adapter,
                            LocationAPI* client,
                            const LocationCallbacks& callbacks) :
            LocMsg(),
            mAdapter(adapter),
            mClient(client),
            mCallbacks(callbacks) {}
        inline virtual void proc() const {
            // check whether we need to notify client of cached location system info
            mAdapter.notifyClientOfCachedLocationSystemInfo(mClient, mCallbacks);
            mAdapter.saveClient(mClient, mCallbacks);
        }
    };

    sendMsg(new MsgAddClient(*this, client, callbacks));
}

void
GnssAdapter::removeClientCommand(LocationAPI* client,
                                 removeClientCompleteCallback rmClientCb)
{
    LOC_LOGD("%s]: client %p", __func__, client);

    struct MsgRemoveClient : public LocMsg {
        GnssAdapter& mAdapter;
        LocationAPI* mClient;
        removeClientCompleteCallback mRmClientCb;
        inline MsgRemoveClient(GnssAdapter& adapter,
                               LocationAPI* client,
                               removeClientCompleteCallback rmCb) :
            LocMsg(),
            mAdapter(adapter),
            mClient(client),
            mRmClientCb(rmCb){}
        inline virtual void proc() const {
            mAdapter.stopClientSessions(mClient);
            mAdapter.eraseClient(mClient);
            if (nullptr != mRmClientCb) {
                mRmClientCb(mClient);
            }
        }
    };

    sendMsg(new MsgRemoveClient(*this, client, rmClientCb));
}

void
GnssAdapter::stopClientSessions(LocationAPI* client)
{
    LOC_LOGD("%s]: client %p", __func__, client);
    for (auto it = mTrackingSessions.begin(); it != mTrackingSessions.end();) {
        if (client == it->first.client) {
            stopTrackingMultiplex(it->first.client, it->first.id);
            it = mTrackingSessions.erase(it);
            continue;
        }
        ++it; // increment only when not erasing an iterator
    }

}

void
GnssAdapter::updateClientsEventMask()
{
    LOC_API_ADAPTER_EVENT_MASK_T mask = 0;
    for (auto it=mClientData.begin(); it != mClientData.end(); ++it) {
        if (it->second.trackingCb != nullptr ||
            it->second.gnssLocationInfoCb != nullptr ||
            it->second.engineLocationsInfoCb != nullptr) {
            mask |= LOC_API_ADAPTER_BIT_PARSED_POSITION_REPORT;
        }
        if (it->second.gnssNiCb != nullptr) {
            mask |= LOC_API_ADAPTER_BIT_NI_NOTIFY_VERIFY_REQUEST;
        }
        if (it->second.gnssSvCb != nullptr) {
            mask |= LOC_API_ADAPTER_BIT_SATELLITE_REPORT;
        }
        if ((it->second.gnssNmeaCb != nullptr) && (mNmeaMask)) {
            mask |= LOC_API_ADAPTER_BIT_NMEA_1HZ_REPORT;
        }
        if (it->second.gnssMeasurementsCb != nullptr) {
            mask |= LOC_API_ADAPTER_BIT_GNSS_MEASUREMENT;
        }
        if (it->second.gnssDataCb != nullptr) {
            mask |= LOC_API_ADAPTER_BIT_PARSED_POSITION_REPORT;
            mask |= LOC_API_ADAPTER_BIT_NMEA_1HZ_REPORT;
            updateNmeaMask(mNmeaMask | LOC_NMEA_MASK_DEBUG_V02);
        }
    }

    /*
    ** For Automotive use cases we need to enable MEASUREMENT, POLY and EPHEMERIS
    ** when QDR is enabled (e.g.: either enabled via conf file or
    ** engine hub is loaded successfully).
    ** Note: this need to be called from msg queue thread.
    */
    if((1 == ContextBase::mGps_conf.EXTERNAL_DR_ENABLED) ||
       (true == initEngHubProxy())) {
        mask |= LOC_API_ADAPTER_BIT_GNSS_MEASUREMENT;
        mask |= LOC_API_ADAPTER_BIT_GNSS_SV_POLYNOMIAL_REPORT;
        mask |= LOC_API_ADAPTER_BIT_PARSED_UNPROPAGATED_POSITION_REPORT;
        mask |= LOC_API_ADAPTER_BIT_GNSS_SV_EPHEMERIS_REPORT;
        mask |= LOC_API_ADAPTER_BIT_LOC_SYSTEM_INFO;
        mask |= LOC_API_ADAPTER_BIT_EVENT_REPORT_INFO;

        // Nhz measurement bit is set based on callback from loc eng hub
        // for Nhz engines.
        mask |= checkMask(LOC_API_ADAPTER_BIT_GNSS_NHZ_MEASUREMENT);

        LOC_LOGd("Auto usecase, Enable MEAS/POLY/EPHEMERIS - mask 0x%" PRIx64 "",
                mask);
    }


    if (mAgpsCbInfo.statusV4Cb != NULL) {
        mask |= LOC_API_ADAPTER_BIT_LOCATION_SERVER_REQUEST;
    }

    // Add ODCPI handling
    if (nullptr != mOdcpiRequestCb) {
        mask |= LOC_API_ADAPTER_BIT_REQUEST_WIFI;
    }

    // need to register for leap second info
    // for proper nmea generation
    mask |= LOC_API_ADAPTER_BIT_LOC_SYSTEM_INFO;
    updateEvtMask(mask, LOC_REGISTRATION_MASK_SET);
}

void
GnssAdapter::handleEngineUpEvent()
{
    LOC_LOGD("%s]: ", __func__);

    struct MsgHandleEngineUpEvent : public LocMsg {
        GnssAdapter& mAdapter;
        inline MsgHandleEngineUpEvent(GnssAdapter& adapter) :
            LocMsg(),
            mAdapter(adapter) {}
        virtual void proc() const {
            mAdapter.broadcastCapabilities(mAdapter.getCapabilities());
            mAdapter.gnssSvIdConfigUpdate();
            mAdapter.gnssSvTypeConfigUpdate();
            mAdapter.gnssSecondaryBandConfigUpdate();
            mAdapter.updatePowerState(mAdapter.getPowerState());

            // restart sessions
            mAdapter.restartSessions(true);
        }
    };

    readConfigCommand();
    setConfigCommand();
    sendMsg(new MsgHandleEngineUpEvent(*this));
}

void
GnssAdapter::restartSessions(bool modemSSR)
{
    LOC_LOGi(":enter");

    if (modemSSR) {
        // odcpi session is no longer active after restart
        mOdcpiRequestActive = false;
    }

    // SPE will be restarted now, so set this variable to false.
    mSPEAlreadyRunningAtHighestInterval = false;

    if (!mTrackingSessions.empty()) {
        // inform engine hub that GNSS session is about to start
        mEngHubProxy->gnssSetFixMode(mLocPositionMode);
        mEngHubProxy->gnssStartFix();
    }

    checkAndRestartSPESession();
}

// suspend all on-going sessions
void
GnssAdapter::suspendSessions()
{
    LOC_LOGi(":enter");

    if (0 != mTrackingSessions.size()) {
        // inform engine hub that GNSS session has stopped
        mEngHubProxy->gnssStopFix();
        mLocApi->stopFix(nullptr);
        mSPEAlreadyRunningAtHighestInterval = false;
    }
}

void GnssAdapter::checkAndRestartSPESession()
{
    LOC_LOGD("%s]: ", __func__);

    if (mTrackingSessions.empty()) {
        return;
    }

    // get the LocationOptions that has the smallest interval, which should be the active one
    TrackingOptions smallestIntervalOptions; // size is zero until set for the first time
    TrackingOptions highestPowerTrackingOptions;
    memset(&smallestIntervalOptions, 0, sizeof(smallestIntervalOptions));
    memset(&highestPowerTrackingOptions, 0, sizeof(highestPowerTrackingOptions));
    for (auto it = mTrackingSessions.begin(); it != mTrackingSessions.end(); ++it) {
        // size of zero means we havent set it yet
        if (0 == smallestIntervalOptions.size ||
            it->second.minInterval < smallestIntervalOptions.minInterval) {
             smallestIntervalOptions = it->second;
        }
        GnssPowerMode powerMode = it->second.powerMode;
        // Size of zero means we havent set it yet
        if (0 == highestPowerTrackingOptions.size ||
            (GNSS_POWER_MODE_INVALID != powerMode &&
                    powerMode < highestPowerTrackingOptions.powerMode)) {
             highestPowerTrackingOptions = it->second;
        }
    }

    LocPosMode locPosMode = {};
    highestPowerTrackingOptions.setLocationOptions(smallestIntervalOptions);
    convertOptions(locPosMode, highestPowerTrackingOptions);

    // want to run SPE session at a fixed min interval in some automotive scenarios
    if(!checkAndSetSPEToRunforNHz(locPosMode)) {
        mLocApi->startFix(locPosMode, nullptr);
    }
}

void
GnssAdapter::requestCapabilitiesCommand(LocationAPI* client)
{
    LOC_LOGD("%s]: ", __func__);

    struct MsgRequestCapabilities : public LocMsg {
        GnssAdapter& mAdapter;
        LocationAPI* mClient;
        inline MsgRequestCapabilities(GnssAdapter& adapter,
                                      LocationAPI* client) :
            LocMsg(),
            mAdapter(adapter),
            mClient(client) {}
        inline virtual void proc() const {
            LocationCallbacks callbacks = mAdapter.getClientCallbacks(mClient);
            if (callbacks.capabilitiesCb == nullptr) {
                LOC_LOGE("%s]: capabilitiesCb is NULL", __func__);
                return;
            }

                LocationCapabilitiesMask mask = mAdapter.getCapabilities();
                callbacks.capabilitiesCb(mask);
        }
    };

    if (ContextBase::isEngineCapabilitiesKnown()) {
        sendMsg(new MsgRequestCapabilities(*this, client));
    }
}

LocationCapabilitiesMask
GnssAdapter::getCapabilities()
{
    LocationCapabilitiesMask mask = 0;
    uint32_t carrierCapabilities = ContextBase::getCarrierCapabilities();
    // time based tracking always supported
    mask |= LOCATION_CAPABILITIES_TIME_BASED_TRACKING_BIT;
    // geofence always supported
    mask |= LOCATION_CAPABILITIES_GEOFENCE_BIT;
    if (carrierCapabilities & LOC_GPS_CAPABILITY_MSB) {
        mask |= LOCATION_CAPABILITIES_GNSS_MSB_BIT;
    }
    if (LOC_GPS_CAPABILITY_MSA & carrierCapabilities) {
        mask |= LOCATION_CAPABILITIES_GNSS_MSA_BIT;
    }
    if (ContextBase::isMessageSupported(LOC_API_ADAPTER_MESSAGE_DISTANCE_BASE_LOCATION_BATCHING)) {
        mask |= LOCATION_CAPABILITIES_TIME_BASED_BATCHING_BIT |
                LOCATION_CAPABILITIES_DISTANCE_BASED_BATCHING_BIT;
    }
    if (ContextBase::isMessageSupported(LOC_API_ADAPTER_MESSAGE_DISTANCE_BASE_TRACKING)) {
        mask |= LOCATION_CAPABILITIES_DISTANCE_BASED_TRACKING_BIT;
    }
    if (ContextBase::isMessageSupported(LOC_API_ADAPTER_MESSAGE_OUTDOOR_TRIP_BATCHING)) {
        mask |= LOCATION_CAPABILITIES_OUTDOOR_TRIP_BATCHING_BIT;
    }
    if (ContextBase::gnssConstellationConfig()) {
        mask |= LOCATION_CAPABILITIES_GNSS_MEASUREMENTS_BIT;
    }
    if (ContextBase::isFeatureSupported(LOC_SUPPORTED_FEATURE_DEBUG_NMEA_V02)) {
        mask |= LOCATION_CAPABILITIES_DEBUG_NMEA_BIT;
    }
    if (ContextBase::isFeatureSupported(LOC_SUPPORTED_FEATURE_CONSTELLATION_ENABLEMENT_V02)) {
        mask |= LOCATION_CAPABILITIES_CONSTELLATION_ENABLEMENT_BIT;
    }
    if (ContextBase::isFeatureSupported(LOC_SUPPORTED_FEATURE_AGPM_V02)) {
        mask |= LOCATION_CAPABILITIES_AGPM_BIT;
    }
    if (ContextBase::isFeatureSupported(LOC_SUPPORTED_FEATURE_LOCATION_PRIVACY)) {
        mask |= LOCATION_CAPABILITIES_PRIVACY_BIT;
    }
    if (ContextBase::isFeatureSupported(LOC_SUPPORTED_FEATURE_MEASUREMENTS_CORRECTION)) {
        mask |= LOCATION_CAPABILITIES_MEASUREMENTS_CORRECTION_BIT;
    }
    if (ContextBase::isFeatureSupported(LOC_SUPPORTED_FEATURE_ROBUST_LOCATION)) {
        mask |= LOCATION_CAPABILITIES_CONFORMITY_INDEX_BIT;
    }
    if (ContextBase::isFeatureSupported(LOC_SUPPORTED_FEATURE_EDGNSS)) {
        mask |= LOCATION_CAPABILITIES_EDGNSS_BIT;
    }
    return mask;
}

void
GnssAdapter::broadcastCapabilities(LocationCapabilitiesMask mask)
{
    for (auto clientData : mClientData) {
        if (nullptr != clientData.second.capabilitiesCb) {
            clientData.second.capabilitiesCb(mask);
        }
    }
}

LocationCallbacks
GnssAdapter::getClientCallbacks(LocationAPI* client)
{
    LocationCallbacks callbacks = {};
    auto it = mClientData.find(client);
    if (it != mClientData.end()) {
        callbacks = it->second;
    }
    return callbacks;
}

void
GnssAdapter::saveClient(LocationAPI* client, const LocationCallbacks& callbacks)
{
    mClientData[client] = callbacks;
    updateClientsEventMask();
}

void
GnssAdapter::notifyClientOfCachedLocationSystemInfo(
        LocationAPI* client, const LocationCallbacks& callbacks) {

    if (mLocSystemInfo.systemInfoMask) {
        // client need to be notified if client has not yet previously registered
        // for the info but now register for it.
        bool notifyClientOfSystemInfo = false;
        // check whether we need to notify client of cached location system info
        //
        // client need to be notified if client has not yet previously registered
        // for the info but now register for it.
        if (callbacks.locationSystemInfoCb) {
            notifyClientOfSystemInfo = true;
            auto it = mClientData.find(client);
            if (it != mClientData.end()) {
                LocationCallbacks oldCallbacks = it->second;
                if (oldCallbacks.locationSystemInfoCb) {
                    notifyClientOfSystemInfo = false;
                }
            }
        }

        if (notifyClientOfSystemInfo) {
            callbacks.locationSystemInfoCb(mLocSystemInfo);
        }
    }
}

void
GnssAdapter::eraseClient(LocationAPI* client)
{
    auto it = mClientData.find(client);
    if (it != mClientData.end()) {
        mClientData.erase(it);
    }
    updateClientsEventMask();
}

bool
GnssAdapter::hasCallbacksToStartTracking(LocationAPI* client)
{
    bool allowed = false;
    auto it = mClientData.find(client);
    if (it != mClientData.end()) {
        if (it->second.trackingCb || it->second.gnssLocationInfoCb ||
                it->second.engineLocationsInfoCb || it->second.gnssMeasurementsCb ||
                it->second.gnssDataCb || it->second.gnssSvCb || it->second.gnssNmeaCb) {
            allowed = true;
        } else {
            LOC_LOGi("missing right callback to start tracking")
        }
    } else {
        LOC_LOGi("client %p not found", client)
    }
    return allowed;
}

bool
GnssAdapter::isTrackingSession(LocationAPI* client, uint32_t sessionId)
{
    LocationSessionKey key(client, sessionId);
    return (mTrackingSessions.find(key) != mTrackingSessions.end());
}

void
GnssAdapter::saveTrackingSession(LocationAPI* client, uint32_t sessionId,
                                 const TrackingOptions& trackingOptions)
{
    LocationSessionKey key(client, sessionId);
    mTrackingSessions[key] = trackingOptions;
}

void
GnssAdapter::eraseTrackingSession(LocationAPI* client, uint32_t sessionId)
{
    LocationSessionKey key(client, sessionId);
    auto itr = mTrackingSessions.find(key);
    if (itr != mTrackingSessions.end()) {
        mTrackingSessions.erase(itr);
    }
}

bool GnssAdapter::setLocPositionMode(const LocPosMode& mode) {
    if (!mLocPositionMode.equals(mode)) {
        mLocPositionMode = mode;
        return true;
    } else {
        return false;
    }
}

void
GnssAdapter::reportResponse(LocationAPI* client, LocationError err, uint32_t sessionId)
{
    LOC_LOGD("%s]: client %p id %u err %u", __func__, client, sessionId, err);

    auto it = mClientData.find(client);
    if (it != mClientData.end() && it->second.responseCb != nullptr) {
        it->second.responseCb(err, sessionId);
    } else {
        LOC_LOGW("%s]: client %p id %u not found in data", __func__, client, sessionId);
    }
}

void
GnssAdapter::reportResponse(LocationError err, uint32_t sessionId)
{
    LOC_LOGD("%s]: id %u err %u", __func__, sessionId, err);

    if (mControlCallbacks.size > 0 && mControlCallbacks.responseCb != nullptr) {
        mControlCallbacks.responseCb(err, sessionId);
    } else {
        LOC_LOGW("%s]: control client response callback not found", __func__);
    }
}

void
GnssAdapter::reportResponse(size_t count, LocationError* errs, uint32_t* ids)
{
    IF_LOC_LOGD {
        std::string idsString = "[";
        std::string errsString = "[";
        if (NULL != ids && NULL != errs) {
            for (size_t i=0; i < count; ++i) {
                idsString += std::to_string(ids[i]) + " ";
                errsString += std::to_string(errs[i]) + " ";
            }
        }
        idsString += "]";
        errsString += "]";

        LOC_LOGD("%s]: ids %s errs %s",
                 __func__, idsString.c_str(), errsString.c_str());
    }

    if (mControlCallbacks.size > 0 && mControlCallbacks.collectiveResponseCb != nullptr) {
        mControlCallbacks.collectiveResponseCb(count, errs, ids);
    } else {
        LOC_LOGW("%s]: control client callback not found", __func__);
    }
}

uint32_t
GnssAdapter::startTrackingCommand(LocationAPI* client, TrackingOptions& options)
{
    uint32_t sessionId = generateSessionId();
    LOC_LOGD("%s]: client %p id %u minInterval %u minDistance %u mode %u powermode %u tbm %u",
             __func__, client, sessionId, options.minInterval, options.minDistance, options.mode,
             options.powerMode, options.tbm);

    struct MsgStartTracking : public LocMsg {
        GnssAdapter& mAdapter;
        LocApiBase& mApi;
        LocationAPI* mClient;
        uint32_t mSessionId;
        mutable TrackingOptions mTrackingOptions;
        inline MsgStartTracking(GnssAdapter& adapter,
                               LocApiBase& api,
                               LocationAPI* client,
                               uint32_t sessionId,
                               TrackingOptions trackingOptions) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api),
            mClient(client),
            mSessionId(sessionId),
            mTrackingOptions(trackingOptions) {}
        inline virtual void proc() const {
            LocationError err = LOCATION_ERROR_SUCCESS;
            if (!mAdapter.hasCallbacksToStartTracking(mClient)) {
                err = LOCATION_ERROR_CALLBACK_MISSING;
            } else if (0 == mTrackingOptions.size) {
                err = LOCATION_ERROR_INVALID_PARAMETER;
            } else {
                if (GNSS_POWER_MODE_INVALID != mTrackingOptions.powerMode &&
                        !ContextBase::isFeatureSupported(LOC_SUPPORTED_FEATURE_AGPM_V02)) {
                    LOC_LOGv("Ignoring power mode, feature not supported.");
                    mTrackingOptions.powerMode = GNSS_POWER_MODE_INVALID;
                }
                if (ContextBase::isFeatureSupported(LOC_SUPPORTED_FEATURE_AGPM_V02) &&
                        GNSS_POWER_MODE_M4 == mTrackingOptions.powerMode &&
                        mTrackingOptions.tbm > TRACKING_TBM_THRESHOLD_MILLIS) {
                    LOC_LOGd("TBM (%d) > %d Falling back to M2 power mode",
                            mTrackingOptions.tbm, TRACKING_TBM_THRESHOLD_MILLIS);
                    mTrackingOptions.powerMode = GNSS_POWER_MODE_M2;
                }
                if (mTrackingOptions.minInterval < MIN_TRACKING_INTERVAL) {
                    mTrackingOptions.minInterval = MIN_TRACKING_INTERVAL;
                }
                // Api doesn't support multiple clients for time based tracking, so mutiplex
                bool reportToClientWithNoWait =
                        mAdapter.startTrackingMultiplex(mClient, mSessionId, mTrackingOptions);
                mAdapter.saveTrackingSession(mClient, mSessionId, mTrackingOptions);

                if (reportToClientWithNoWait) {
                    mAdapter.reportResponse(mClient, LOCATION_ERROR_SUCCESS, mSessionId);
                }
            }
        }
    };

    sendMsg(new MsgStartTracking(*this, *mLocApi, client, sessionId, options));
    return sessionId;

}

bool
GnssAdapter::startTrackingMultiplex(LocationAPI* client, uint32_t sessionId,
                                    const TrackingOptions& options)
{
    bool reportToClientWithNoWait = true;

    if (mTrackingSessions.empty()) {
        /*Reset previous NMEA reported time stamp */
        mPrevNmeaRptTimeNsec = 0;
        startTracking(client, sessionId, options);
        // need to wait for QMI callback
        reportToClientWithNoWait = false;
    } else {
        // find the smallest interval and powerMode
        TrackingOptions multiplexedOptions = {}; // size is 0 until set for the first time
        GnssPowerMode multiplexedPowerMode = GNSS_POWER_MODE_INVALID;
        memset(&multiplexedOptions, 0, sizeof(multiplexedOptions));
        for (auto it = mTrackingSessions.begin(); it != mTrackingSessions.end(); ++it) {
            // if not set or there is a new smallest interval, then set the new interval
            if (0 == multiplexedOptions.size ||
                it->second.minInterval < multiplexedOptions.minInterval) {
                multiplexedOptions = it->second;
            }
            // if session is not the one we are updating and either powerMode
            // is not set or there is a new smallest powerMode, then set the new powerMode
            if (GNSS_POWER_MODE_INVALID == multiplexedPowerMode ||
                it->second.powerMode < multiplexedPowerMode) {
                multiplexedPowerMode = it->second.powerMode;
            }
        }
        bool updateOptions = false;
        // if session we are starting has smaller interval then next smallest
        if (options.minInterval < multiplexedOptions.minInterval) {
            multiplexedOptions.minInterval = options.minInterval;
            updateOptions = true;
        }

        // if session we are starting has smaller powerMode then next smallest
        if (options.powerMode < multiplexedPowerMode) {
            multiplexedOptions.powerMode = options.powerMode;
            updateOptions = true;
        }
        if (updateOptions) {
            // restart time based tracking with the newly updated options

            startTracking(client, sessionId, multiplexedOptions);
            // need to wait for QMI callback
            reportToClientWithNoWait = false;
        }
        // else part: no QMI call is made, need to report back to client right away
    }

    return reportToClientWithNoWait;
}

void
GnssAdapter::startTracking(LocationAPI* client, uint32_t sessionId,
        const TrackingOptions& trackingOptions)
{
    LOC_LOGd("minInterval %u minDistance %u mode %u powermode %u tbm %u",
         trackingOptions.minInterval, trackingOptions.minDistance,
         trackingOptions.mode, trackingOptions.powerMode, trackingOptions.tbm);

    LocPosMode locPosMode = {};
    convertOptions(locPosMode, trackingOptions);
    // save position mode parameters
    setLocPositionMode(locPosMode);
    // inform engine hub that GNSS session is about to start
    mEngHubProxy->gnssSetFixMode(mLocPositionMode);
    mEngHubProxy->gnssStartFix();

    if (!checkAndSetSPEToRunforNHz(locPosMode)) {
        mLocApi->startFix(locPosMode, new LocApiResponse(*getContext(),
                          [this, client, sessionId] (LocationError err) {
                if (LOCATION_ERROR_SUCCESS != err) {
                    eraseTrackingSession(client, sessionId);
                }

                reportResponse(client, err, sessionId);
            }
        ));
    } else {
        reportResponse(client, LOCATION_ERROR_SUCCESS, sessionId);
    }

}

void
GnssAdapter::updateTracking(LocationAPI* client, uint32_t sessionId,
        const TrackingOptions& updatedOptions, const TrackingOptions& oldOptions)
{
    LocPosMode locPosMode = {};
    convertOptions(locPosMode, updatedOptions);
    // save position mode parameters
    setLocPositionMode(locPosMode);

    // inform engine hub that GNSS session is about to start
    mEngHubProxy->gnssSetFixMode(mLocPositionMode);
    mEngHubProxy->gnssStartFix();

    // want to run SPE session at a fixed min interval in some automotive scenarios
    if(!checkAndSetSPEToRunforNHz(locPosMode)) {
        mLocApi->startFix(locPosMode, new LocApiResponse(*getContext(),
                          [this, client, sessionId, oldOptions] (LocationError err) {
                if (LOCATION_ERROR_SUCCESS != err) {
                    // restore the old LocationOptions
                    saveTrackingSession(client, sessionId, oldOptions);
                }

                reportResponse(client, err, sessionId);
            }
        ));
    } else {
        reportResponse(client, LOCATION_ERROR_SUCCESS, sessionId);
    }
}

void
GnssAdapter::setPositionModeCommand(LocPosMode& locPosMode)
{
    LOC_LOGD("%s]: min_interval %u mode %u",
             __func__, locPosMode.min_interval, locPosMode.mode);

    struct MsgSetPositionMode : public LocMsg {
        GnssAdapter& mAdapter;
        LocApiBase& mApi;
        LocPosMode mLocPosMode;
        inline MsgSetPositionMode(GnssAdapter& adapter,
                                  LocApiBase& api,
                                  LocPosMode& locPosMode) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api),
            mLocPosMode(locPosMode) {}
        inline virtual void proc() const {
             // saves the mode in adapter to be used when startTrackingCommand is called from ULP
            if (mAdapter.setLocPositionMode(mLocPosMode)) {
                mAdapter.mEngHubProxy->gnssSetFixMode(mLocPosMode);
                mApi.setPositionMode(mLocPosMode);
            }
        }
    };

    sendMsg(new MsgSetPositionMode(*this, *mLocApi, locPosMode));
}

void
GnssAdapter::updateTrackingOptionsCommand(LocationAPI* client, uint32_t id,
                                          TrackingOptions& options)
{
    LOC_LOGD("%s]: client %p id %u minInterval %u mode %u",
             __func__, client, id, options.minInterval, options.mode);

    struct MsgUpdateTracking : public LocMsg {
        GnssAdapter& mAdapter;
        LocApiBase& mApi;
        LocationAPI* mClient;
        uint32_t mSessionId;
        mutable TrackingOptions mTrackingOptions;
        inline MsgUpdateTracking(GnssAdapter& adapter,
                                LocApiBase& api,
                                LocationAPI* client,
                                uint32_t sessionId,
                                TrackingOptions trackingOptions) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api),
            mClient(client),
            mSessionId(sessionId),
            mTrackingOptions(trackingOptions) {}
        inline virtual void proc() const {
            if (mAdapter.isTrackingSession(mClient, mSessionId)) {
                LocationError err = LOCATION_ERROR_SUCCESS;
                if (0 == mTrackingOptions.size) {
                    err = LOCATION_ERROR_INVALID_PARAMETER;
                } else {
                    if (GNSS_POWER_MODE_INVALID != mTrackingOptions.powerMode &&
                            !ContextBase::isFeatureSupported(LOC_SUPPORTED_FEATURE_AGPM_V02)) {
                        LOC_LOGv("Ignoring power mode, feature not supported.");
                        mTrackingOptions.powerMode = GNSS_POWER_MODE_INVALID;
                    }
                    if (ContextBase::isFeatureSupported(LOC_SUPPORTED_FEATURE_AGPM_V02) &&
                            GNSS_POWER_MODE_M4 == mTrackingOptions.powerMode &&
                            mTrackingOptions.tbm > TRACKING_TBM_THRESHOLD_MILLIS) {
                        LOC_LOGd("TBM (%d) > %d Falling back to M2 power mode",
                                mTrackingOptions.tbm, TRACKING_TBM_THRESHOLD_MILLIS);
                        mTrackingOptions.powerMode = GNSS_POWER_MODE_M2;
                    }
                    if (mTrackingOptions.minInterval < MIN_TRACKING_INTERVAL) {
                        mTrackingOptions.minInterval = MIN_TRACKING_INTERVAL;
                    }
                    // Api doesn't support multiple clients for time based tracking, so mutiplex
                    bool reportToClientWithNoWait =
                            mAdapter.updateTrackingMultiplex(mClient, mSessionId, mTrackingOptions);
                    mAdapter.saveTrackingSession(mClient, mSessionId, mTrackingOptions);

                    if (reportToClientWithNoWait) {
                        mAdapter.reportResponse(mClient, err, mSessionId);
                    }
                }
            }
            // we do not reportResponse for the case where there is no existing tracking session
            // for the client and id being used, since updateTrackingCommand can be sent to both
            // GnssAdapter & FlpAdapter by LocationAPI and we want to avoid incorrect error response
        }
    };

    sendMsg(new MsgUpdateTracking(*this, *mLocApi, client, id, options));
}

bool
GnssAdapter::updateTrackingMultiplex(LocationAPI* client, uint32_t id,
                                     const TrackingOptions& trackingOptions)
{
    bool reportToClientWithNoWait = true;

    LocationSessionKey key(client, id);
    // get the session we are updating
    auto it = mTrackingSessions.find(key);

    // cache the clients existing LocationOptions
    TrackingOptions oldOptions = it->second;

    // if session we are updating exists and the minInterval or powerMode has changed
    if (it != mTrackingSessions.end() && (it->second.minInterval != trackingOptions.minInterval ||
        it->second.powerMode != trackingOptions.powerMode)) {
        // find the smallest interval and powerMode, other than the session we are updating
        TrackingOptions multiplexedOptions = {}; // size is 0 until set for the first time
        GnssPowerMode multiplexedPowerMode = GNSS_POWER_MODE_INVALID;
        memset(&multiplexedOptions, 0, sizeof(multiplexedOptions));
        for (auto it2 = mTrackingSessions.begin(); it2 != mTrackingSessions.end(); ++it2) {
            // if session is not the one we are updating and either interval
            // is not set or there is a new smallest interval, then set the new interval
            if (it2->first != key && (0 == multiplexedOptions.size ||
                it2->second.minInterval < multiplexedOptions.minInterval)) {
                 multiplexedOptions = it2->second;
            }
            // if session is not the one we are updating and either powerMode
            // is not set or there is a new smallest powerMode, then set the new powerMode
            if (it2->first != key && (GNSS_POWER_MODE_INVALID == multiplexedPowerMode ||
                it2->second.powerMode < multiplexedPowerMode)) {
                multiplexedPowerMode = it2->second.powerMode;
            }
            // else part: no QMI call is made, need to report back to client right away
        }
        bool updateOptions = false;
        // if session we are updating has smaller interval then next smallest
        if (trackingOptions.minInterval < multiplexedOptions.minInterval) {
            multiplexedOptions.minInterval = trackingOptions.minInterval;
            updateOptions = true;
        }
        // if session we are updating has smaller powerMode then next smallest
        if (trackingOptions.powerMode < multiplexedPowerMode) {
            multiplexedOptions.powerMode = trackingOptions.powerMode;
            updateOptions = true;
        }
        // if only one session exists, then tracking should be updated with it
        if (1 == mTrackingSessions.size()) {
            multiplexedOptions = trackingOptions;
            updateOptions = true;
        }
        if (updateOptions) {
            // restart time based tracking with the newly updated options
            updateTracking(client, id, multiplexedOptions, oldOptions);
            // need to wait for QMI callback
            reportToClientWithNoWait = false;
        }
    }

    return reportToClientWithNoWait;
}

void
GnssAdapter::stopTrackingCommand(LocationAPI* client, uint32_t id)
{
    LOC_LOGD("%s]: client %p id %u", __func__, client, id);

    struct MsgStopTracking : public LocMsg {
        GnssAdapter& mAdapter;
        LocApiBase& mApi;
        LocationAPI* mClient;
        uint32_t mSessionId;
        inline MsgStopTracking(GnssAdapter& adapter,
                               LocApiBase& api,
                               LocationAPI* client,
                               uint32_t sessionId) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api),
            mClient(client),
            mSessionId(sessionId) {}
        inline virtual void proc() const {
            if (mAdapter.isTrackingSession(mClient, mSessionId)) {
                // Api doesn't support multiple clients for time based tracking, so mutiplex
                bool reportToClientWithNoWait =
                        mAdapter.stopTrackingMultiplex(mClient, mSessionId);
                mAdapter.eraseTrackingSession(mClient, mSessionId);

                if (reportToClientWithNoWait) {
                    mAdapter.reportResponse(mClient, LOCATION_ERROR_SUCCESS, mSessionId);
                }
            }
            // we do not reportResponse for the case where there is no existing tracking session
            // for the client and id being used, since stopTrackingCommand can be sent to both
            // GnssAdapter & FlpAdapter by LocationAPI and we want to avoid incorrect error response
        }
    };

    sendMsg(new MsgStopTracking(*this, *mLocApi, client, id));
}

bool
GnssAdapter::stopTrackingMultiplex(LocationAPI* client, uint32_t id)
{
    bool reportToClientWithNoWait = true;

    if (1 == mTrackingSessions.size()) {
        stopTracking(client, id);
        // need to wait for QMI callback
        reportToClientWithNoWait = false;
    } else {
        LocationSessionKey key(client, id);

        // get the session we are stopping
        auto it = mTrackingSessions.find(key);
        if (it != mTrackingSessions.end()) {
            // find the smallest interval and powerMode, other than the session we are stopping
            TrackingOptions multiplexedOptions = {}; // size is 0 until set for the first time
            GnssPowerMode multiplexedPowerMode = GNSS_POWER_MODE_INVALID;
            memset(&multiplexedOptions, 0, sizeof(multiplexedOptions));
            for (auto it2 = mTrackingSessions.begin(); it2 != mTrackingSessions.end(); ++it2) {
                // if session is not the one we are stopping and either interval
                // is not set or there is a new smallest interval, then set the new interval
                if (it2->first != key && (0 == multiplexedOptions.size ||
                    it2->second.minInterval < multiplexedOptions.minInterval)) {
                     multiplexedOptions = it2->second;
                }
                // if session is not the one we are stopping and either powerMode
                // is not set or there is a new smallest powerMode, then set the new powerMode
                if (it2->first != key && (GNSS_POWER_MODE_INVALID == multiplexedPowerMode ||
                    it2->second.powerMode < multiplexedPowerMode)) {
                    multiplexedPowerMode = it2->second.powerMode;
                }
            }
            // if session we are stopping has smaller interval then next smallest or
            // if session we are stopping has smaller powerMode then next smallest
            if (it->second.minInterval < multiplexedOptions.minInterval ||
                it->second.powerMode < multiplexedPowerMode) {
                multiplexedOptions.powerMode = multiplexedPowerMode;
                // restart time based tracking with the newly updated options
                startTracking(client, id, multiplexedOptions);
                // need to wait for QMI callback
                reportToClientWithNoWait = false;
            }
            // else part: no QMI call is made, need to report back to client right away
        }
    }

    return reportToClientWithNoWait;
}

void
GnssAdapter::stopTracking(LocationAPI* client, uint32_t id)
{
    // inform engine hub that GNSS session has stopped
    mEngHubProxy->gnssStopFix();

    mLocApi->stopFix(new LocApiResponse(*getContext(),
                     [this, client, id] (LocationError err) {
        reportResponse(client, err, id);
    }));

    mSPEAlreadyRunningAtHighestInterval = false;
}

bool
GnssAdapter::hasNiNotifyCallback(LocationAPI* client)
{
    auto it = mClientData.find(client);
    return (it != mClientData.end() && it->second.gnssNiCb);
}

void
GnssAdapter::gnssNiResponseCommand(LocationAPI* client,
                                   uint32_t id,
                                   GnssNiResponse response)
{
    LOC_LOGD("%s]: client %p id %u response %u", __func__, client, id, response);

    struct MsgGnssNiResponse : public LocMsg {
        GnssAdapter& mAdapter;
        LocationAPI* mClient;
        uint32_t mSessionId;
        GnssNiResponse mResponse;
        inline MsgGnssNiResponse(GnssAdapter& adapter,
                                 LocationAPI* client,
                                 uint32_t sessionId,
                                 GnssNiResponse response) :
            LocMsg(),
            mAdapter(adapter),
            mClient(client),
            mSessionId(sessionId),
            mResponse(response) {}
        inline virtual void proc() const {
            NiData& niData = mAdapter.getNiData();
            LocationError err = LOCATION_ERROR_SUCCESS;
            if (!mAdapter.hasNiNotifyCallback(mClient)) {
                err = LOCATION_ERROR_ID_UNKNOWN;
            } else {
                NiSession* pSession = NULL;
                if (mSessionId == niData.sessionEs.reqID &&
                    NULL != niData.sessionEs.rawRequest) {
                    pSession = &niData.sessionEs;
                    // ignore any SUPL NI non-Es session if a SUPL NI ES is accepted
                    if (mResponse == GNSS_NI_RESPONSE_ACCEPT &&
                        NULL != niData.session.rawRequest) {
                            pthread_mutex_lock(&niData.session.tLock);
                            niData.session.resp = GNSS_NI_RESPONSE_IGNORE;
                            niData.session.respRecvd = true;
                            pthread_cond_signal(&niData.session.tCond);
                            pthread_mutex_unlock(&niData.session.tLock);
                    }
                } else if (mSessionId == niData.session.reqID &&
                    NULL != niData.session.rawRequest) {
                    pSession = &niData.session;
                }

                if (pSession) {
                    LOC_LOGI("%s]: gnssNiResponseCommand: send user mResponse %u for id %u",
                             __func__, mResponse, mSessionId);
                    pthread_mutex_lock(&pSession->tLock);
                    pSession->resp = mResponse;
                    pSession->respRecvd = true;
                    pthread_cond_signal(&pSession->tCond);
                    pthread_mutex_unlock(&pSession->tLock);
                } else {
                    err = LOCATION_ERROR_ID_UNKNOWN;
                    LOC_LOGE("%s]: gnssNiResponseCommand: id %u not an active session",
                             __func__, mSessionId);
                }
            }
            mAdapter.reportResponse(mClient, err, mSessionId);
        }
    };

    sendMsg(new MsgGnssNiResponse(*this, client, id, response));

}

void
GnssAdapter::gnssNiResponseCommand(GnssNiResponse response, void* rawRequest)
{
    LOC_LOGD("%s]: response %u", __func__, response);

    struct MsgGnssNiResponse : public LocMsg {
        GnssAdapter& mAdapter;
        LocApiBase& mApi;
        const GnssNiResponse mResponse;
        const void* mPayload;
        inline MsgGnssNiResponse(GnssAdapter& adapter,
                                 LocApiBase& api,
                                 const GnssNiResponse response,
                                 const void* rawRequest) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api),
            mResponse(response),
            mPayload(rawRequest) {}
        inline virtual ~MsgGnssNiResponse() {
        }
        inline virtual void proc() const {
            const void *rawPayload = mPayload;
            mApi.informNiResponse(mResponse, mPayload);
        }
    };

    sendMsg(new MsgGnssNiResponse(*this, *mLocApi, response, rawRequest));

}

uint32_t
GnssAdapter::enableCommand(LocationTechnologyType techType)
{
    uint32_t sessionId = generateSessionId();
    LOC_LOGD("%s]: id %u techType %u", __func__, sessionId, techType);

    struct MsgEnableGnss : public LocMsg {
        GnssAdapter& mAdapter;
        LocApiBase& mApi;
        ContextBase& mContext;
        uint32_t mSessionId;
        LocationTechnologyType mTechType;
        inline MsgEnableGnss(GnssAdapter& adapter,
                             LocApiBase& api,
                             ContextBase& context,
                             uint32_t sessionId,
                             LocationTechnologyType techType) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api),
            mContext(context),
            mSessionId(sessionId),
            mTechType(techType) {}
        inline virtual void proc() const {
            LocationError err = LOCATION_ERROR_SUCCESS;
            uint32_t powerVoteId = mAdapter.getPowerVoteId();
            if (mTechType != LOCATION_TECHNOLOGY_TYPE_GNSS) {
                err = LOCATION_ERROR_INVALID_PARAMETER;
            } else if (powerVoteId > 0) {
                err = LOCATION_ERROR_ALREADY_STARTED;
            } else {
                mContext.modemPowerVote(true);
                mAdapter.setPowerVoteId(mSessionId);

                mApi.sendMsg(new LocApiMsg([&mApi = mApi] () {
                    mApi.setGpsLockSync(GNSS_CONFIG_GPS_LOCK_NONE);
               }));

                mAdapter.mXtraObserver.updateLockStatus(
                        GNSS_CONFIG_GPS_LOCK_NONE);
            }
            mAdapter.reportResponse(err, mSessionId);
        }
    };

    if (mContext != NULL) {
        sendMsg(new MsgEnableGnss(*this, *mLocApi, *mContext, sessionId, techType));
    } else {
        LOC_LOGE("%s]: Context is NULL", __func__);
    }

    return sessionId;
}

void
GnssAdapter::disableCommand(uint32_t id)
{
    LOC_LOGD("%s]: id %u", __func__, id);

    struct MsgDisableGnss : public LocMsg {
        GnssAdapter& mAdapter;
        LocApiBase& mApi;
        ContextBase& mContext;
        uint32_t mSessionId;
        inline MsgDisableGnss(GnssAdapter& adapter,
                             LocApiBase& api,
                             ContextBase& context,
                             uint32_t sessionId) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api),
            mContext(context),
            mSessionId(sessionId) {}
        inline virtual void proc() const {
            LocationError err = LOCATION_ERROR_SUCCESS;
            uint32_t powerVoteId = mAdapter.getPowerVoteId();
            if (powerVoteId != mSessionId) {
                err = LOCATION_ERROR_ID_UNKNOWN;
            } else {
                mContext.modemPowerVote(false);
                mAdapter.setPowerVoteId(0);

                GnssConfigGpsLock gpsLock =
                    ContextBase::mGps_conf.GPS_LOCK;
                mApi.sendMsg(new LocApiMsg([&mApi = mApi,gpsLock] () {
                    mApi.setGpsLockSync(gpsLock);
                }));
                mAdapter.mXtraObserver.updateLockStatus(
                        ContextBase::mGps_conf.GPS_LOCK);
            }
            mAdapter.reportResponse(err, mSessionId);
        }
    };

    if (mContext != NULL) {
        sendMsg(new MsgDisableGnss(*this, *mLocApi, *mContext, id));
    }

}

// This function computes the VRP based latitude, longitude and alittude, and
// north, east and up velocity and save the result into EHubTechReport.
void
GnssAdapter::computeVRPBasedLla(const UlpLocation& loc, GpsLocationExtended& locExt,
                                const LeverArmConfigInfo& leverArmConfigInfo) {

    float leverArm[3];
    float rollPitchYaw[3];
    double lla[3];

    uint16_t locFlags = loc.gpsLocation.flags;
    uint64_t locExtFlags = locExt.flags;

    // check for SPE fix
    if (!((locExtFlags & GPS_LOCATION_EXTENDED_HAS_OUTPUT_ENG_TYPE) &&
          (locExt.locOutputEngType == LOC_OUTPUT_ENGINE_SPE))){
        LOC_LOGv("not SPE fix, return");
        return;
    }

    // we can only do translation if we have VRP based lever ARM info
    LeverArmTypeMask leverArmFlags = leverArmConfigInfo.leverArmValidMask;
    if (!(leverArmFlags & LEVER_ARM_TYPE_GNSS_TO_VRP_BIT)) {
        LOC_LOGd("no VRP based lever ARM info");
        return;
    }

    leverArm[0] = leverArmConfigInfo.gnssToVRP.forwardOffsetMeters;
    leverArm[1] = leverArmConfigInfo.gnssToVRP.sidewaysOffsetMeters;
    leverArm[2] = leverArmConfigInfo.gnssToVRP.upOffsetMeters;

    if ((locFlags & LOC_GPS_LOCATION_HAS_LAT_LONG) &&
        (locFlags & LOC_GPS_LOCATION_HAS_ALTITUDE) &&
        (locFlags & LOCATION_HAS_BEARING_BIT)) {

        lla[0] = loc.gpsLocation.latitude * DEG2RAD;
        lla[1] = loc.gpsLocation.longitude * DEG2RAD;
        lla[2] = loc.gpsLocation.altitude;

        rollPitchYaw[0] = 0.0f;
        rollPitchYaw[1] = 0.0f;
        rollPitchYaw[2] = loc.gpsLocation.bearing * DEG2RAD;

        loc_convert_lla_gnss_to_vrp(lla, rollPitchYaw, leverArm);

        // assign the converted value into position report and
        // set up valid mask
        locExt.llaVRPBased.latitude  = lla[0] * RAD2DEG;
        locExt.llaVRPBased.longitude = lla[1] * RAD2DEG;
        locExt.llaVRPBased.altitude  = lla[2];
        locExt.flags |= GPS_LOCATION_EXTENDED_HAS_LLA_VRP_BASED;
    } else {
        LOC_LOGd("SPE fix missing latitude/longitude/alitutde");
        return;
    }
}

void
GnssAdapter::reportPositionEvent(const UlpLocation& ulpLocation,
                                 const GpsLocationExtended& locationExtended,
                                 enum loc_sess_status status,
                                 LocPosTechMask techMask,
                                 GnssDataNotification* pDataNotify,
                                 int msInWeek)
{
    // this position is from QMI LOC API, then send report to engine hub
    // also, send out SPE fix promptly to the clients that have registered
    // with SPE report
    LOC_LOGd("reportPositionEvent, eng type: %d, unpro %d, sess status %d msInWeek %d",
             locationExtended.locOutputEngType,
             ulpLocation.unpropagatedPosition, status, msInWeek);

    struct MsgReportSPEPosition : public LocMsg {
        GnssAdapter& mAdapter;
        mutable UlpLocation mUlpLocation;
        mutable GpsLocationExtended mLocationExtended;
        enum loc_sess_status mStatus;
        LocPosTechMask mTechMask;
        mutable GnssDataNotification mDataNotify;
        int mMsInWeek;

        inline MsgReportSPEPosition(GnssAdapter& adapter,
                                    const UlpLocation& ulpLocation,
                                    const GpsLocationExtended& locationExtended,
                                    enum loc_sess_status status,
                                    LocPosTechMask techMask,
                                    GnssDataNotification dataNotify,
                                    int msInWeek) :
            LocMsg(),
            mAdapter(adapter),
            mUlpLocation(ulpLocation),
            mLocationExtended(locationExtended),
            mStatus(status),
            mTechMask(techMask),
            mDataNotify(dataNotify),
            mMsInWeek(msInWeek) {}
        inline virtual void proc() const {
            if (mAdapter.mTrackingSessions.empty()) {
                LOC_LOGd("reportPositionEvent, no session on-going, throw away the SPE reports");
                return;
            }

            if (false == mUlpLocation.unpropagatedPosition && mDataNotify.size != 0) {
                if (mMsInWeek >= 0) {
                    mAdapter.getDataInformation((GnssDataNotification&)mDataNotify,
                                                mMsInWeek);
                }
                mAdapter.reportData(mDataNotify);
            }

            if (true == mAdapter.initEngHubProxy()){
                // send the SPE fix to engine hub
                mAdapter.mEngHubProxy->gnssReportPosition(mUlpLocation, mLocationExtended, mStatus);
                // report out all SPE fix if it is not propagated, even for failed fix
                if (false == mUlpLocation.unpropagatedPosition) {
                    EngineLocationInfo engLocationInfo = {};
                    engLocationInfo.location = mUlpLocation;
                    engLocationInfo.locationExtended = mLocationExtended;
                    engLocationInfo.sessionStatus = mStatus;

                    // obtain the VRP based latitude/longitude/altitude for SPE fix
                    computeVRPBasedLla(engLocationInfo.location,
                                       engLocationInfo.locationExtended,
                                       mAdapter.mLocConfigInfo.leverArmConfigInfo);
                    mAdapter.reportEnginePositions(1, &engLocationInfo);
                }
                return;
            }

            // unpropagated report: is only for engine hub to consume and no need
            // to send out to the clients
            if (true == mUlpLocation.unpropagatedPosition) {
                return;
            }

            // extract bug report info - this returns true if consumed by systemstatus
            SystemStatus* s = mAdapter.getSystemStatus();
            if ((nullptr != s) &&
                    ((LOC_SESS_SUCCESS == mStatus) || (LOC_SESS_INTERMEDIATE == mStatus))){
                s->eventPosition(mUlpLocation, mLocationExtended);
            }

            mAdapter.reportPosition(mUlpLocation, mLocationExtended, mStatus, mTechMask);
        }
    };

    if (mContext != NULL) {
        GnssDataNotification dataNotifyCopy = {};
        if (pDataNotify) {
            dataNotifyCopy = *pDataNotify;
            dataNotifyCopy.size = sizeof(dataNotifyCopy);
        }
        sendMsg(new MsgReportSPEPosition(*this, ulpLocation, locationExtended,
                                          status, techMask, dataNotifyCopy, msInWeek));
    }
}

void
GnssAdapter::reportEnginePositionsEvent(unsigned int count,
                                        EngineLocationInfo* locationArr)
{
    struct MsgReportEnginePositions : public LocMsg {
        GnssAdapter& mAdapter;
        unsigned int mCount;
        EngineLocationInfo mEngLocInfo[LOC_OUTPUT_ENGINE_COUNT];
        inline MsgReportEnginePositions(GnssAdapter& adapter,
                                        unsigned int count,
                                        EngineLocationInfo* locationArr) :
            LocMsg(),
            mAdapter(adapter),
            mCount(count) {
            if (mCount > LOC_OUTPUT_ENGINE_COUNT) {
                mCount = LOC_OUTPUT_ENGINE_COUNT;
            }
            if (mCount > 0) {
                memcpy(mEngLocInfo, locationArr, sizeof(EngineLocationInfo)*mCount);
            }
        }
        inline virtual void proc() const {
            mAdapter.reportEnginePositions(mCount, mEngLocInfo);
        }
    };

    sendMsg(new MsgReportEnginePositions(*this, count, locationArr));
}

bool
GnssAdapter::needReport(const UlpLocation& ulpLocation,
                        enum loc_sess_status status,
                        LocPosTechMask techMask) {
    bool reported = false;

    // if engine hub is enabled, aka, any of the engine services is enabled,
    // then always output position reported by engine hub to requesting client
    if (true == initEngHubProxy()) {
        reported = true;
    } else {
        reported = LocApiBase::needReport(ulpLocation, status, techMask);
    }
    return reported;
}

bool GnssAdapter::needToGenerateNmeaReport(const uint32_t &gpsTimeOfWeekMs,
        const struct timespec32_t &apTimeStamp)
{
    bool retVal = false;
    uint64_t currentTimeNsec = 0;

    if (NMEA_PROVIDER_AP == ContextBase::mGps_conf.NMEA_PROVIDER && !mTrackingSessions.empty()) {
        currentTimeNsec = (apTimeStamp.tv_sec * BILLION_NSEC + apTimeStamp.tv_nsec);
        if ((GNSS_NMEA_REPORT_RATE_NHZ == ContextBase::sNmeaReportRate) ||
                (GPS_DEFAULT_FIX_INTERVAL_MS <= mLocPositionMode.min_interval)) {
            retVal = true;
        } else { /*tbf is less than 1000 milli-seconds and NMEA reporting rate is set to 1Hz */
            /* Always send NMEA string for first position report
             * Send when gpsTimeOfWeekMs is closely aligned with integer boundary
             */
            if ((0 == mPrevNmeaRptTimeNsec) ||
                (0 != gpsTimeOfWeekMs) && (NMEA_MIN_THRESHOLD_MSEC >= (gpsTimeOfWeekMs % 1000))) {
                retVal = true;
            } else {
                uint64_t timeDiffMsec = ((currentTimeNsec - mPrevNmeaRptTimeNsec) / 1000000);
                // Send when the delta time becomes >= 1 sec
                if (NMEA_MAX_THRESHOLD_MSEC <= timeDiffMsec) {
                    retVal = true;
                }
            }
        }
        if (true == retVal) {
            mPrevNmeaRptTimeNsec = currentTimeNsec;
        }
    }
    return retVal;
}

// only fused report (when engine hub is enabled) or
// SPE report (when engine hub is disabled) will reach this function
void
GnssAdapter::reportPosition(const UlpLocation& ulpLocation,
                            const GpsLocationExtended& locationExtended,
                            enum loc_sess_status status,
                            LocPosTechMask techMask)
{
    bool reported = needReport(ulpLocation, status, techMask);
    mGnssSvIdUsedInPosAvail = false;
    mGnssMbSvIdUsedInPosAvail = false;

    LOC_LOGd("needReport %d, status %d, eng type %d, eng hub inited %d", reported, status,
             locationExtended.locOutputEngType, initEngHubProxy());

    if (reported) {
        if (locationExtended.flags & GPS_LOCATION_EXTENDED_HAS_GNSS_SV_USED_DATA) {
            mGnssSvIdUsedInPosAvail = true;
            mGnssSvIdUsedInPosition = locationExtended.gnss_sv_used_ids;
            if (locationExtended.flags & GPS_LOCATION_EXTENDED_HAS_MULTIBAND) {
                mGnssMbSvIdUsedInPosAvail = true;
                mGnssMbSvIdUsedInPosition = locationExtended.gnss_mb_sv_used_ids;
            }
        }

        GnssLocationInfoNotification locationInfo = {};
        convertLocationInfo(locationInfo, locationExtended);
        convertLocation(locationInfo.location, ulpLocation, locationExtended, techMask);

        for (auto it=mClientData.begin(); it != mClientData.end(); ++it) {
            if ((nullptr != it->second.engineLocationsInfoCb) &&
                (false == initEngHubProxy())) {
                // if engine hub is disabled, this is SPE fix from modem
                // we need to have one copy marked as fused and leave the other copy
                // unmodified (which is marked as SPE fix in LocAPIV02.cpp) and
                // dispatch both copies to the engineLocationsInfoCb
                GnssLocationInfoNotification engLocationsInfo[2];
                engLocationsInfo[0] = locationInfo;
                engLocationsInfo[0].locOutputEngType = LOC_OUTPUT_ENGINE_FUSED;
                engLocationsInfo[0].flags |= GNSS_LOCATION_INFO_OUTPUT_ENG_TYPE_BIT;
                engLocationsInfo[1] = locationInfo;
                it->second.engineLocationsInfoCb(2, engLocationsInfo);
            }else if (nullptr != it->second.gnssLocationInfoCb) {
                it->second.gnssLocationInfoCb(locationInfo);
            } else if (nullptr != it->second.trackingCb) {
                it->second.trackingCb(locationInfo.location);
            }
        }

        // if PACE is enabled
        if ((true == mLocConfigInfo.paceConfigInfo.isValid) &&
                (true == mLocConfigInfo.paceConfigInfo.enable)) {
            // If fix has sensor contribution, and it is fused fix with DRE engine
            // contributing to the fix, inject to modem
            if ((LOC_POS_TECH_MASK_SENSORS & techMask) &&
                    (locationInfo.flags & GNSS_LOCATION_INFO_OUTPUT_ENG_TYPE_BIT) &&
                    (locationInfo.locOutputEngType == LOC_OUTPUT_ENGINE_FUSED) &&
                    (locationInfo.flags & GNSS_LOCATION_INFO_OUTPUT_ENG_MASK_BIT) &&
                    (locationInfo.locOutputEngMask & DEAD_RECKONING_ENGINE)) {
                mLocApi->injectPosition(locationInfo, false);
            }
        }
    }

    if (needToGenerateNmeaReport(locationExtended.gpsTime.gpsTimeOfWeekMs,
            locationExtended.timeStamp.apTimeStamp)) {
        /*Only BlankNMEA sentence needs to be processed and sent, if both lat, long is 0 &
          horReliability is not set. */
        bool blank_fix = ((0 == ulpLocation.gpsLocation.latitude) &&
                          (0 == ulpLocation.gpsLocation.longitude) &&
                          (LOC_RELIABILITY_NOT_SET == locationExtended.horizontal_reliability));
        uint8_t generate_nmea = (reported && status != LOC_SESS_FAILURE && !blank_fix);
        bool custom_nmea_gga = (1 == ContextBase::mGps_conf.CUSTOM_NMEA_GGA_FIX_QUALITY_ENABLED);
        std::vector<std::string> nmeaArraystr;
        loc_nmea_generate_pos(ulpLocation, locationExtended, mLocSystemInfo,
                              generate_nmea, custom_nmea_gga, nmeaArraystr);
        stringstream ss;
        for (auto itor = nmeaArraystr.begin(); itor != nmeaArraystr.end(); ++itor) {
            ss << *itor;
        }
        string s = ss.str();
        reportNmea(s.c_str(), s.length());
    }
}

void
GnssAdapter::reportEnginePositions(unsigned int count,
                                   const EngineLocationInfo* locationArr)
{
    bool needReportEnginePositions = false;
    for (auto it=mClientData.begin(); it != mClientData.end(); ++it) {
        if (nullptr != it->second.engineLocationsInfoCb) {
            needReportEnginePositions = true;
            break;
        }
    }

    GnssLocationInfoNotification locationInfo[LOC_OUTPUT_ENGINE_COUNT] = {};
    for (unsigned int i = 0; i < count; i++) {
        const EngineLocationInfo* engLocation = (locationArr+i);
        // if it is fused/default location, call reportPosition maintain legacy behavior
        if ((GPS_LOCATION_EXTENDED_HAS_OUTPUT_ENG_TYPE & engLocation->locationExtended.flags) &&
            (LOC_OUTPUT_ENGINE_FUSED == engLocation->locationExtended.locOutputEngType)) {
            reportPosition(engLocation->location,
                           engLocation->locationExtended,
                           engLocation->sessionStatus,
                           engLocation->location.tech_mask);
        }

        if (needReportEnginePositions) {
            convertLocationInfo(locationInfo[i], engLocation->locationExtended);
            convertLocation(locationInfo[i].location,
                            engLocation->location,
                            engLocation->locationExtended,
                            engLocation->location.tech_mask);
        }
    }

    if (needReportEnginePositions) {
        for (auto it=mClientData.begin(); it != mClientData.end(); ++it) {
            if (nullptr != it->second.engineLocationsInfoCb) {
                it->second.engineLocationsInfoCb(count, locationInfo);
            }
        }
    }
}

void
GnssAdapter::reportSvEvent(const GnssSvNotification& svNotify,
                           bool fromEngineHub)
{
    if (!fromEngineHub) {
        mEngHubProxy->gnssReportSv(svNotify);
        if (true == initEngHubProxy()){
            return;
        }
    }

    struct MsgReportSv : public LocMsg {
        GnssAdapter& mAdapter;
        const GnssSvNotification mSvNotify;
        inline MsgReportSv(GnssAdapter& adapter,
                           const GnssSvNotification& svNotify) :
            LocMsg(),
            mAdapter(adapter),
            mSvNotify(svNotify) {}
        inline virtual void proc() const {
            mAdapter.reportSv((GnssSvNotification&)mSvNotify);
        }
    };

    sendMsg(new MsgReportSv(*this, svNotify));
}

void
GnssAdapter::reportSv(GnssSvNotification& svNotify)
{
    int numSv = svNotify.count;
    uint16_t gnssSvId = 0;
    uint64_t svUsedIdMask = 0;
    for (int i=0; i < numSv; i++) {
        svUsedIdMask = 0;
        gnssSvId = svNotify.gnssSvs[i].svId;
        GnssSignalTypeMask signalTypeMask = svNotify.gnssSvs[i].gnssSignalTypeMask;
        switch (svNotify.gnssSvs[i].type) {
            case GNSS_SV_TYPE_GPS:
                if (mGnssSvIdUsedInPosAvail) {
                    if (mGnssMbSvIdUsedInPosAvail) {
                        switch (signalTypeMask) {
                        case GNSS_SIGNAL_GPS_L1CA:
                            svUsedIdMask = mGnssMbSvIdUsedInPosition.gps_l1ca_sv_used_ids_mask;
                            break;
                        case GNSS_SIGNAL_GPS_L1C:
                            svUsedIdMask = mGnssMbSvIdUsedInPosition.gps_l1c_sv_used_ids_mask;
                            break;
                        case GNSS_SIGNAL_GPS_L2:
                            svUsedIdMask = mGnssMbSvIdUsedInPosition.gps_l2_sv_used_ids_mask;
                            break;
                        case GNSS_SIGNAL_GPS_L5:
                            svUsedIdMask = mGnssMbSvIdUsedInPosition.gps_l5_sv_used_ids_mask;
                            break;
                        }
                    } else {
                        svUsedIdMask = mGnssSvIdUsedInPosition.gps_sv_used_ids_mask;
                    }
                }
                break;
            case GNSS_SV_TYPE_GLONASS:
                if (mGnssSvIdUsedInPosAvail) {
                    if (mGnssMbSvIdUsedInPosAvail) {
                        switch (signalTypeMask) {
                        case GNSS_SIGNAL_GLONASS_G1:
                            svUsedIdMask = mGnssMbSvIdUsedInPosition.glo_g1_sv_used_ids_mask;
                            break;
                        case GNSS_SIGNAL_GLONASS_G2:
                            svUsedIdMask = mGnssMbSvIdUsedInPosition.glo_g2_sv_used_ids_mask;
                            break;
                        }
                    } else {
                        svUsedIdMask = mGnssSvIdUsedInPosition.glo_sv_used_ids_mask;
                    }
                }
                // map the svid to respective constellation range 1..xx
                // then repective constellation svUsedIdMask map correctly to svid
                gnssSvId = gnssSvId - GLO_SV_PRN_MIN + 1;
                break;
            case GNSS_SV_TYPE_BEIDOU:
                if (mGnssSvIdUsedInPosAvail) {
                    if (mGnssMbSvIdUsedInPosAvail) {
                        switch (signalTypeMask) {
                        case GNSS_SIGNAL_BEIDOU_B1I:
                            svUsedIdMask = mGnssMbSvIdUsedInPosition.bds_b1i_sv_used_ids_mask;
                            break;
                        case GNSS_SIGNAL_BEIDOU_B1C:
                            svUsedIdMask = mGnssMbSvIdUsedInPosition.bds_b1c_sv_used_ids_mask;
                            break;
                        case GNSS_SIGNAL_BEIDOU_B2I:
                            svUsedIdMask = mGnssMbSvIdUsedInPosition.bds_b2i_sv_used_ids_mask;
                            break;
                        case GNSS_SIGNAL_BEIDOU_B2AI:
                            svUsedIdMask = mGnssMbSvIdUsedInPosition.bds_b2ai_sv_used_ids_mask;
                            break;
                        case GNSS_SIGNAL_BEIDOU_B2AQ:
                            svUsedIdMask = mGnssMbSvIdUsedInPosition.bds_b2aq_sv_used_ids_mask;
                            break;
                        }
                    } else {
                        svUsedIdMask = mGnssSvIdUsedInPosition.bds_sv_used_ids_mask;
                    }
                }
                gnssSvId = gnssSvId - BDS_SV_PRN_MIN + 1;
                break;
            case GNSS_SV_TYPE_GALILEO:
                if (mGnssSvIdUsedInPosAvail) {
                    if (mGnssMbSvIdUsedInPosAvail) {
                        switch (signalTypeMask) {
                        case GNSS_SIGNAL_GALILEO_E1:
                            svUsedIdMask = mGnssMbSvIdUsedInPosition.gal_e1_sv_used_ids_mask;
                            break;
                        case GNSS_SIGNAL_GALILEO_E5A:
                            svUsedIdMask = mGnssMbSvIdUsedInPosition.gal_e5a_sv_used_ids_mask;
                            break;
                        case GNSS_SIGNAL_GALILEO_E5B:
                            svUsedIdMask = mGnssMbSvIdUsedInPosition.gal_e5b_sv_used_ids_mask;
                            break;
                        }
                    } else {
                        svUsedIdMask = mGnssSvIdUsedInPosition.gal_sv_used_ids_mask;
                    }
                }
                gnssSvId = gnssSvId - GAL_SV_PRN_MIN + 1;
                break;
            case GNSS_SV_TYPE_QZSS:
                if (mGnssSvIdUsedInPosAvail) {
                    if (mGnssMbSvIdUsedInPosAvail) {
                        switch (signalTypeMask) {
                        case GNSS_SIGNAL_QZSS_L1CA:
                            svUsedIdMask = mGnssMbSvIdUsedInPosition.qzss_l1ca_sv_used_ids_mask;
                            break;
                        case GNSS_SIGNAL_QZSS_L1S:
                            svUsedIdMask = mGnssMbSvIdUsedInPosition.qzss_l1s_sv_used_ids_mask;
                            break;
                        case GNSS_SIGNAL_QZSS_L2:
                            svUsedIdMask = mGnssMbSvIdUsedInPosition.qzss_l2_sv_used_ids_mask;
                            break;
                        case GNSS_SIGNAL_QZSS_L5:
                            svUsedIdMask = mGnssMbSvIdUsedInPosition.qzss_l5_sv_used_ids_mask;
                            break;
                        }
                    } else {
                        svUsedIdMask = mGnssSvIdUsedInPosition.qzss_sv_used_ids_mask;
                    }
                }
                gnssSvId = gnssSvId - QZSS_SV_PRN_MIN + 1;
                break;
            case GNSS_SV_TYPE_NAVIC:
                if (mGnssSvIdUsedInPosAvail) {
                    svUsedIdMask = mGnssSvIdUsedInPosition.navic_sv_used_ids_mask;
                }
                gnssSvId = gnssSvId - NAVIC_SV_PRN_MIN + 1;
                break;
            default:
                svUsedIdMask = 0;
                break;
        }

        // If SV ID was used in previous position fix, then set USED_IN_FIX
        // flag, else clear the USED_IN_FIX flag.
        if (svFitsMask(svUsedIdMask, gnssSvId) && (svUsedIdMask & (1ULL << (gnssSvId - 1)))) {
            svNotify.gnssSvs[i].gnssSvOptionsMask |= GNSS_SV_OPTIONS_USED_IN_FIX_BIT;
        }
    }

    for (auto it=mClientData.begin(); it != mClientData.end(); ++it) {
        if (nullptr != it->second.gnssSvCb) {
            it->second.gnssSvCb(svNotify);
        }
    }

    if (NMEA_PROVIDER_AP == ContextBase::mGps_conf.NMEA_PROVIDER && !mTrackingSessions.empty()) {
        std::vector<std::string> nmeaArraystr;
        loc_nmea_generate_sv(svNotify, nmeaArraystr);
        stringstream ss;
        for (auto itor = nmeaArraystr.begin(); itor != nmeaArraystr.end(); ++itor) {
            ss << *itor;
        }
        string s = ss.str();
        reportNmea(s.c_str(), s.length());
    }

    mGnssSvIdUsedInPosAvail = false;
}

void
GnssAdapter::reportNmeaEvent(const char* nmea, size_t length)
{
    if (NMEA_PROVIDER_AP == ContextBase::mGps_conf.NMEA_PROVIDER &&
        !loc_nmea_is_debug(nmea, length)) {
        return;
    }

    struct MsgReportNmea : public LocMsg {
        GnssAdapter& mAdapter;
        const char* mNmea;
        size_t mLength;
        inline MsgReportNmea(GnssAdapter& adapter,
                             const char* nmea,
                             size_t length) :
            LocMsg(),
            mAdapter(adapter),
            mNmea(new char[length+1]),
            mLength(length) {
                if (mNmea == nullptr) {
                    LOC_LOGE("%s] new allocation failed, fatal error.", __func__);
                    return;
                }
                strlcpy((char*)mNmea, nmea, length+1);
            }
        inline virtual ~MsgReportNmea()
        {
            delete[] mNmea;
        }
        inline virtual void proc() const {
            // extract bug report info - this returns true if consumed by systemstatus
            bool ret = false;
            SystemStatus* s = mAdapter.getSystemStatus();
            if (nullptr != s) {
                ret = s->setNmeaString(mNmea, mLength);
            }
            if (false == ret) {
                // forward NMEA message to upper layer
                mAdapter.reportNmea(mNmea, mLength);
            }
        }
    };

    sendMsg(new MsgReportNmea(*this, nmea, length));
}

void
GnssAdapter::reportNmea(const char* nmea, size_t length)
{
    GnssNmeaNotification nmeaNotification = {};
    nmeaNotification.size = sizeof(GnssNmeaNotification);

    struct timeval tv;
    gettimeofday(&tv, (struct timezone *) NULL);
    int64_t now = tv.tv_sec * 1000LL + tv.tv_usec / 1000;
    nmeaNotification.timestamp = now;
    nmeaNotification.nmea = nmea;
    nmeaNotification.length = length;

    for (auto it=mClientData.begin(); it != mClientData.end(); ++it) {
        if (nullptr != it->second.gnssNmeaCb) {
            it->second.gnssNmeaCb(nmeaNotification);
        }
    }
}

void
GnssAdapter::reportData(GnssDataNotification& dataNotify)
{
    for (int sig = 0; sig < GNSS_LOC_MAX_NUMBER_OF_SIGNAL_TYPES; sig++) {
        if (GNSS_LOC_DATA_JAMMER_IND_BIT ==
            (dataNotify.gnssDataMask[sig] & GNSS_LOC_DATA_JAMMER_IND_BIT)) {
            LOC_LOGv("jammerInd[%d]=%f", sig, dataNotify.jammerInd[sig]);
        }
        if (GNSS_LOC_DATA_AGC_BIT ==
            (dataNotify.gnssDataMask[sig] & GNSS_LOC_DATA_AGC_BIT)) {
            LOC_LOGv("agc[%d]=%f", sig, dataNotify.agc[sig]);
        }
    }
    for (auto it = mClientData.begin(); it != mClientData.end(); ++it) {
        if (nullptr != it->second.gnssDataCb) {
            it->second.gnssDataCb(dataNotify);
        }
    }
}

bool
GnssAdapter::requestNiNotifyEvent(const GnssNiNotification &notify, const void* data)
{
    LOC_LOGI("%s]: notif_type: %d, timeout: %d, default_resp: %d"
             "requestor_id: %s (encoding: %d) text: %s text (encoding: %d) extras: %s",
             __func__, notify.type, notify.timeout, notify.timeoutResponse,
             notify.requestor, notify.requestorEncoding,
             notify.message, notify.messageEncoding, notify.extras);

    struct MsgReportNiNotify : public LocMsg {
        GnssAdapter& mAdapter;
        const GnssNiNotification mNotify;
        const void* mData;
        inline MsgReportNiNotify(GnssAdapter& adapter,
                                 const GnssNiNotification& notify,
                                 const void* data) :
            LocMsg(),
            mAdapter(adapter),
            mNotify(notify),
            mData(data) {}
        inline virtual void proc() const {
            mAdapter.requestNiNotify(mNotify, mData);
        }
    };

    sendMsg(new MsgReportNiNotify(*this, notify, data));

    return true;
}

void
GnssAdapter::reportLocationSystemInfoEvent(const LocationSystemInfo & locationSystemInfo) {

    // send system info to engine hub
    mEngHubProxy->gnssReportSystemInfo(locationSystemInfo);

    struct MsgLocationSystemInfo : public LocMsg {
        GnssAdapter& mAdapter;
        LocationSystemInfo mSystemInfo;
        inline MsgLocationSystemInfo(GnssAdapter& adapter,
            const LocationSystemInfo& systemInfo) :
            LocMsg(),
            mAdapter(adapter),
            mSystemInfo(systemInfo) {}
        inline virtual void proc() const {
            mAdapter.reportLocationSystemInfo(mSystemInfo);
        }
    };

    sendMsg(new MsgLocationSystemInfo(*this, locationSystemInfo));
}

void
GnssAdapter::reportLocationSystemInfo(const LocationSystemInfo & locationSystemInfo) {
    // save the info into the master copy piece by piece, as other system info
    // may come at different time
    if (locationSystemInfo.systemInfoMask & LOCATION_SYS_INFO_LEAP_SECOND) {
        mLocSystemInfo.systemInfoMask |= LOCATION_SYS_INFO_LEAP_SECOND;

        const LeapSecondSystemInfo &srcLeapSecondSysInfo = locationSystemInfo.leapSecondSysInfo;
        LeapSecondSystemInfo &dstLeapSecondSysInfo = mLocSystemInfo.leapSecondSysInfo;
        if (srcLeapSecondSysInfo.leapSecondInfoMask &
                LEAP_SECOND_SYS_INFO_CURRENT_LEAP_SECONDS_BIT) {
            dstLeapSecondSysInfo.leapSecondInfoMask |=
                LEAP_SECOND_SYS_INFO_CURRENT_LEAP_SECONDS_BIT;
            dstLeapSecondSysInfo.leapSecondCurrent = srcLeapSecondSysInfo.leapSecondCurrent;
        }
        // once leap second change event is complete, modem may send up event invalidate the leap second
        // change info while AP is still processing report during leap second transition
        // so, we choose to keep this info around even though it is old
        if (srcLeapSecondSysInfo.leapSecondInfoMask & LEAP_SECOND_SYS_INFO_LEAP_SECOND_CHANGE_BIT) {
            dstLeapSecondSysInfo.leapSecondInfoMask |= LEAP_SECOND_SYS_INFO_LEAP_SECOND_CHANGE_BIT;
            dstLeapSecondSysInfo.leapSecondChangeInfo = srcLeapSecondSysInfo.leapSecondChangeInfo;
        }
    }

    // we received new info, inform client of the newly received info
    if (locationSystemInfo.systemInfoMask) {
        for (auto it=mClientData.begin(); it != mClientData.end(); ++it) {
            if (it->second.locationSystemInfoCb != nullptr) {
                it->second.locationSystemInfoCb(locationSystemInfo);
            }
        }
    }
}

static void* niThreadProc(void *args)
{
    NiSession* pSession = (NiSession*)args;
    int rc = 0;          /* return code from pthread calls */

    struct timespec present_time;
    struct timespec expire_time;

    pthread_mutex_lock(&pSession->tLock);
    /* Calculate absolute expire time */
    clock_gettime(CLOCK_MONOTONIC, &present_time);
    expire_time.tv_sec  = present_time.tv_sec + pSession->respTimeLeft;
    expire_time.tv_nsec = present_time.tv_nsec;
    LOC_LOGD("%s]: time out set for abs time %ld with delay %d sec",
             __func__, (long)expire_time.tv_sec, pSession->respTimeLeft);

    while (!pSession->respRecvd) {
        rc = pthread_cond_timedwait(&pSession->tCond,
                                    &pSession->tLock,
                                    &expire_time);
        if (rc == ETIMEDOUT) {
            pSession->resp = GNSS_NI_RESPONSE_NO_RESPONSE;
            LOC_LOGD("%s]: time out after valting for specified time. Ret Val %d",
                     __func__, rc);
            break;
        }
    }
    LOC_LOGD("%s]: Java layer has sent us a user response and return value from "
             "pthread_cond_timedwait = %d pSession->resp is %u", __func__, rc, pSession->resp);
    pSession->respRecvd = false; /* Reset the user response flag for the next session*/

    // adding this check to support modem restart, in which case, we need the thread
    // to exit without calling sending data. We made sure that rawRequest is NULL in
    // loc_eng_ni_reset_on_engine_restart()
    GnssAdapter* adapter = pSession->adapter;
    GnssNiResponse resp;
    void* rawRequest = NULL;
    bool sendResponse = false;

    if (NULL != pSession->rawRequest) {
        if (pSession->resp != GNSS_NI_RESPONSE_IGNORE) {
            resp = pSession->resp;
            rawRequest = pSession->rawRequest;
            sendResponse = true;
        } else {
            free(pSession->rawRequest);
        }
        pSession->rawRequest = NULL;
    }
    pthread_mutex_unlock(&pSession->tLock);

    pSession->respTimeLeft = 0;
    pSession->reqID = 0;

    if (sendResponse) {
        adapter->gnssNiResponseCommand(resp, rawRequest);
    }

    return NULL;
}

bool
GnssAdapter::requestNiNotify(const GnssNiNotification& notify, const void* data)
{
    NiSession* pSession = NULL;
    gnssNiCallback gnssNiCb = nullptr;

    for (auto it=mClientData.begin(); it != mClientData.end(); ++it) {
        if (nullptr != it->second.gnssNiCb) {
            gnssNiCb = it->second.gnssNiCb;
            break;
        }
    }
    if (nullptr == gnssNiCb) {
        EXIT_LOG(%s, "no clients with gnssNiCb.");
        return false;
    }

    if (notify.type == GNSS_NI_TYPE_EMERGENCY_SUPL) {
        if (NULL != mNiData.sessionEs.rawRequest) {
            LOC_LOGI("%s]: supl es NI in progress, new supl es NI ignored, type: %d",
                     __func__, notify.type);
            if (NULL != data) {
                free((void*)data);
            }
        } else {
            pSession = &mNiData.sessionEs;
        }
    } else {
        if (NULL != mNiData.session.rawRequest ||
            NULL != mNiData.sessionEs.rawRequest) {
            LOC_LOGI("%s]: supl NI in progress, new supl NI ignored, type: %d",
                     __func__, notify.type);
            if (NULL != data) {
                free((void*)data);
            }
        } else {
            pSession = &mNiData.session;
        }
    }

    if (pSession) {
        /* Save request */
        pSession->rawRequest = (void*)data;
        pSession->reqID = ++mNiData.reqIDCounter;
        pSession->adapter = this;

        int sessionId = pSession->reqID;

        /* For robustness, spawn a thread at this point to timeout to clear up the notification
         * status, even though the OEM layer in java does not do so.
         **/
        pSession->respTimeLeft =
             5 + (notify.timeout != 0 ? notify.timeout : LOC_NI_NO_RESPONSE_TIME);

        int rc = 0;
        rc = pthread_create(&pSession->thread, NULL, niThreadProc, pSession);
        if (rc) {
            LOC_LOGE("%s]: Loc NI thread is not created.", __func__);
        }
        rc = pthread_detach(pSession->thread);
        if (rc) {
            LOC_LOGE("%s]: Loc NI thread is not detached.", __func__);
        }

        if (nullptr != gnssNiCb) {
            gnssNiCb(sessionId, notify);
        }
    }

    return true;
}

void
GnssAdapter::reportGnssMeasurementDataEvent(const GnssMeasurementsNotification& measurements,
                                            int msInWeek)
{
    LOC_LOGD("%s]: msInWeek=%d", __func__, msInWeek);

    struct MsgReportGnssMeasurementData : public LocMsg {
        GnssAdapter& mAdapter;
        GnssMeasurementsNotification mMeasurementsNotify;
        inline MsgReportGnssMeasurementData(GnssAdapter& adapter,
                                            const GnssMeasurementsNotification& measurements,
                                            int msInWeek) :
                LocMsg(),
                mAdapter(adapter),
                mMeasurementsNotify(measurements) {
            if (-1 != msInWeek) {
                mAdapter.getAgcInformation(mMeasurementsNotify, msInWeek);
            }
        }
        inline virtual void proc() const {
            mAdapter.reportGnssMeasurementData(mMeasurementsNotify);
        }
    };

    sendMsg(new MsgReportGnssMeasurementData(*this, measurements, msInWeek));
}

void
GnssAdapter::reportGnssMeasurementData(const GnssMeasurementsNotification& measurements)
{
    for (auto it=mClientData.begin(); it != mClientData.end(); ++it) {
        if (nullptr != it->second.gnssMeasurementsCb) {
            it->second.gnssMeasurementsCb(measurements);
        }
    }
}

void
GnssAdapter::reportDGnssDataUsable(GnssSvMeasurementSet &svMeasurementSet)
{
    uint32_t i;
    bool preDGnssDataUsage = mDGnssDataUsage;

    mDGnssDataUsage = false;
    for (i = 0; i < svMeasurementSet.svMeasCount; i++) {
        Gnss_SVMeasurementStructType& svMeas = svMeasurementSet.svMeas[i];
        if (svMeas.dgnssSvMeas.dgnssMeasStatus) {
            mDGnssDataUsage = true;
            break;
        }
    }
    if (mDGnssDataUsage != preDGnssDataUsage) {
        if (mCdfwInterface) {
            mCdfwInterface->reportUsable(mQDgnssListenerHDL, mDGnssDataUsage);
        }
    }
}

void
GnssAdapter::reportSvMeasurementEvent(GnssSvMeasurementSet &svMeasurementSet)
{
    LOC_LOGD("%s]: ", __func__);
    mEngHubProxy->gnssReportSvMeasurement(svMeasurementSet);
    if (mDGnssNeedReport) {
        reportDGnssDataUsable(svMeasurementSet);
    }
}

void
GnssAdapter::reportSvPolynomialEvent(GnssSvPolynomial &svPolynomial)
{
    LOC_LOGD("%s]: ", __func__);
    mEngHubProxy->gnssReportSvPolynomial(svPolynomial);
}

void
GnssAdapter::reportSvEphemerisEvent(GnssSvEphemerisReport & svEphemeris)
{
    LOC_LOGD("%s]:", __func__);
    mEngHubProxy->gnssReportSvEphemeris(svEphemeris);
}


bool
GnssAdapter::requestOdcpiEvent(OdcpiRequestInfo& request)
{
    struct MsgRequestOdcpi : public LocMsg {
        GnssAdapter& mAdapter;
        OdcpiRequestInfo mOdcpiRequest;
        inline MsgRequestOdcpi(GnssAdapter& adapter, OdcpiRequestInfo& request) :
                LocMsg(),
                mAdapter(adapter),
                mOdcpiRequest(request) {}
        inline virtual void proc() const {
            mAdapter.requestOdcpi(mOdcpiRequest);
        }
    };

    sendMsg(new MsgRequestOdcpi(*this, request));
    return true;
}

void GnssAdapter::requestOdcpi(const OdcpiRequestInfo& request)
{
    if (nullptr != mOdcpiRequestCb) {
        LOC_LOGd("request: type %d, tbf %d, isEmergency %d"
                 " requestActive: %d timerActive: %d",
                 request.type, request.tbfMillis, request.isEmergencyMode,
                 mOdcpiRequestActive, mOdcpiTimer.isActive());
        // ODCPI START and ODCPI STOP from modem can come in quick succession
        // so the mOdcpiTimer helps avoid spamming the framework as well as
        // extending the odcpi session past 30 seconds if needed
        if (ODCPI_REQUEST_TYPE_START == request.type) {
            if (false == mOdcpiRequestActive && false == mOdcpiTimer.isActive()) {
                mOdcpiRequestCb(request);
                mOdcpiRequestActive = true;
                mOdcpiTimer.start();
            // if the current active odcpi session is non-emergency, and the new
            // odcpi request is emergency, replace the odcpi request with new request
            // and restart the timer
            } else if (false == mOdcpiRequest.isEmergencyMode &&
                       true == request.isEmergencyMode) {
                mOdcpiRequestCb(request);
                mOdcpiRequestActive = true;
                if (true == mOdcpiTimer.isActive()) {
                    mOdcpiTimer.restart();
                } else {
                    mOdcpiTimer.start();
                }
            // if ODCPI request is not active but the timer is active, then
            // just update the active state and wait for timer to expire
            // before requesting new ODCPI to avoid spamming ODCPI requests
            } else if (false == mOdcpiRequestActive && true == mOdcpiTimer.isActive()) {
                mOdcpiRequestActive = true;
            }
            mOdcpiRequest = request;
        // the request is being stopped, but allow timer to expire first
        // before stopping the timer just in case more ODCPI requests come
        // to avoid spamming more odcpi requests to the framework
        } else {
            mOdcpiRequestActive = false;
        }
    } else {
        LOC_LOGw("ODCPI request not supported");
    }
}

bool GnssAdapter::reportDeleteAidingDataEvent(GnssAidingData& aidingData)
{
    LOC_LOGD("%s]:", __func__);
    mEngHubProxy->gnssDeleteAidingData(aidingData);
    return true;
}

bool GnssAdapter::reportKlobucharIonoModelEvent(GnssKlobucharIonoModel & ionoModel)
{
    LOC_LOGD("%s]:", __func__);
    mEngHubProxy->gnssReportKlobucharIonoModel(ionoModel);
    return true;
}

bool GnssAdapter::reportGnssAdditionalSystemInfoEvent(
        GnssAdditionalSystemInfo & additionalSystemInfo)
{
    LOC_LOGD("%s]:", __func__);
    mEngHubProxy->gnssReportAdditionalSystemInfo(additionalSystemInfo);
    return true;
}

void GnssAdapter::initOdcpiCommand(const OdcpiRequestCallback& callback)
{
    struct MsgInitOdcpi : public LocMsg {
        GnssAdapter& mAdapter;
        OdcpiRequestCallback mOdcpiCb;
        inline MsgInitOdcpi(GnssAdapter& adapter,
                const OdcpiRequestCallback& callback) :
                LocMsg(),
                mAdapter(adapter),
                mOdcpiCb(callback) {}
        inline virtual void proc() const {
            mAdapter.initOdcpi(mOdcpiCb);
        }
    };

    sendMsg(new MsgInitOdcpi(*this, callback));
}

void GnssAdapter::initOdcpi(const OdcpiRequestCallback& callback)
{
    mOdcpiRequestCb = callback;

    /* Register for WIFI request */
    updateEvtMask(LOC_API_ADAPTER_BIT_REQUEST_WIFI,
            LOC_REGISTRATION_MASK_ENABLED);
}

void GnssAdapter::injectOdcpiCommand(const Location& location)
{
    struct MsgInjectOdcpi : public LocMsg {
        GnssAdapter& mAdapter;
        Location mLocation;
        inline MsgInjectOdcpi(GnssAdapter& adapter, const Location& location) :
                LocMsg(),
                mAdapter(adapter),
                mLocation(location) {}
        inline virtual void proc() const {
            mAdapter.injectOdcpi(mLocation);
        }
    };

    sendMsg(new MsgInjectOdcpi(*this, location));
}

void GnssAdapter::injectOdcpi(const Location& location)
{
    LOC_LOGd("ODCPI Injection: requestActive: %d timerActive: %d"
             "lat %.7f long %.7f",
            mOdcpiRequestActive, mOdcpiTimer.isActive(),
            location.latitude, location.longitude);

    mLocApi->injectPosition(location, true);
}

// Called in the context of LocTimer thread
void OdcpiTimer::timeOutCallback()
{
    if (nullptr != mAdapter) {
        mAdapter->odcpiTimerExpireEvent();
    }
}

// Called in the context of LocTimer thread
void GnssAdapter::odcpiTimerExpireEvent()
{
    struct MsgOdcpiTimerExpire : public LocMsg {
        GnssAdapter& mAdapter;
        inline MsgOdcpiTimerExpire(GnssAdapter& adapter) :
                LocMsg(),
                mAdapter(adapter) {}
        inline virtual void proc() const {
            mAdapter.odcpiTimerExpire();
        }
    };
    sendMsg(new MsgOdcpiTimerExpire(*this));
}
void GnssAdapter::odcpiTimerExpire()
{
    LOC_LOGd("requestActive: %d timerActive: %d",
            mOdcpiRequestActive, mOdcpiTimer.isActive());

    // if ODCPI request is still active after timer
    // expires, request again and restart timer
    if (mOdcpiRequestActive) {
        mOdcpiRequestCb(mOdcpiRequest);
        mOdcpiTimer.restart();
    } else {
        mOdcpiTimer.stop();
    }
}

void
GnssAdapter::invokeGnssEnergyConsumedCallback(uint64_t energyConsumedSinceFirstBoot) {
    if (mGnssEnergyConsumedCb) {
        mGnssEnergyConsumedCb(energyConsumedSinceFirstBoot);
        mGnssEnergyConsumedCb = nullptr;
    }
}

bool
GnssAdapter::reportGnssEngEnergyConsumedEvent(uint64_t energyConsumedSinceFirstBoot){
    LOC_LOGD("%s]: %" PRIu64 " ", __func__, energyConsumedSinceFirstBoot);

    struct MsgReportGnssGnssEngEnergyConsumed : public LocMsg {
        GnssAdapter& mAdapter;
        uint64_t mGnssEnergyConsumedSinceFirstBoot;
        inline MsgReportGnssGnssEngEnergyConsumed(GnssAdapter& adapter,
                                                  uint64_t energyConsumed) :
                LocMsg(),
                mAdapter(adapter),
                mGnssEnergyConsumedSinceFirstBoot(energyConsumed) {}
        inline virtual void proc() const {
            mAdapter.invokeGnssEnergyConsumedCallback(mGnssEnergyConsumedSinceFirstBoot);
        }
    };

    sendMsg(new MsgReportGnssGnssEngEnergyConsumed(*this, energyConsumedSinceFirstBoot));
    return true;
}

void GnssAdapter::initDefaultAgps() {
    LOC_LOGD("%s]: ", __func__);

    void *handle = nullptr;
    if ((handle = dlopen("libloc_net_iface.so", RTLD_NOW)) == nullptr) {
        LOC_LOGD("%s]: libloc_net_iface.so not found !", __func__);
        return;
    }

    LocAgpsGetAgpsCbInfo getAgpsCbInfo = (LocAgpsGetAgpsCbInfo)
            dlsym(handle, "LocNetIfaceAgps_getAgpsCbInfo");
    if (getAgpsCbInfo == nullptr) {
        LOC_LOGE("%s]: Failed to get method LocNetIfaceAgps_getStatusCb", __func__);
        dlclose(handle);
        return;
    }

    AgpsCbInfo& cbInfo = getAgpsCbInfo(agpsOpenResultCb, agpsCloseResultCb, this);

    if (cbInfo.statusV4Cb == nullptr) {
        LOC_LOGE("%s]: statusV4Cb is nullptr!", __func__);
        dlclose(handle);
        return;
    }

    initAgps(cbInfo);
}

void GnssAdapter::initDefaultAgpsCommand() {
    LOC_LOGD("%s]: ", __func__);

    struct MsgInitDefaultAgps : public LocMsg {
        GnssAdapter& mAdapter;
        inline MsgInitDefaultAgps(GnssAdapter& adapter) :
            LocMsg(),
            mAdapter(adapter) {
            }
        inline virtual void proc() const {
            mAdapter.initDefaultAgps();
        }
    };

    sendMsg(new MsgInitDefaultAgps(*this));
}

/* INIT LOC AGPS MANAGER */

void GnssAdapter::initAgps(const AgpsCbInfo& cbInfo) {
    LOC_LOGD("%s]: mAgpsCbInfo.cbPriority - %d;  cbInfo.cbPriority - %d",
            __func__, mAgpsCbInfo.cbPriority, cbInfo.cbPriority)

    if (!((ContextBase::mGps_conf.CAPABILITIES & LOC_GPS_CAPABILITY_MSB) ||
            (ContextBase::mGps_conf.CAPABILITIES & LOC_GPS_CAPABILITY_MSA))) {
        return;
    }

    if (mAgpsCbInfo.cbPriority > cbInfo.cbPriority) {
        return;
    } else {
        mAgpsCbInfo = cbInfo;

        mAgpsManager.registerFrameworkStatusCallback((AgnssStatusIpV4Cb)cbInfo.statusV4Cb);

        mAgpsManager.createAgpsStateMachines();

        /* Register for AGPS event mask */
        updateEvtMask(LOC_API_ADAPTER_BIT_LOCATION_SERVER_REQUEST,
                LOC_REGISTRATION_MASK_ENABLED);
    }
}

void GnssAdapter::initAgpsCommand(const AgpsCbInfo& cbInfo){
    LOC_LOGI("GnssAdapter::initAgpsCommand");

    /* Message to initialize AGPS module */
    struct AgpsMsgInit: public LocMsg {
        const AgpsCbInfo mCbInfo;
        GnssAdapter& mAdapter;

        inline AgpsMsgInit(const AgpsCbInfo& cbInfo,
                GnssAdapter& adapter) :
                LocMsg(), mCbInfo(cbInfo), mAdapter(adapter) {
            LOC_LOGV("AgpsMsgInit");
        }

        inline virtual void proc() const {
            LOC_LOGV("AgpsMsgInit::proc()");
            mAdapter.initAgps(mCbInfo);
        }
    };

    /* Send message to initialize AGPS Manager */
    sendMsg(new AgpsMsgInit(cbInfo, *this));
}

/* GnssAdapter::requestATL
 * Method triggered in QMI thread as part of handling below message:
 * eQMI_LOC_SERVER_REQUEST_OPEN_V02
 * Triggers the AGPS state machine to setup AGPS call for below WWAN types:
 * eQMI_LOC_WWAN_TYPE_INTERNET_V02
 * eQMI_LOC_WWAN_TYPE_AGNSS_V02
 * eQMI_LOC_WWAN_TYPE_AGNSS_EMERGENCY_V02 */
bool GnssAdapter::requestATL(int connHandle, LocAGpsType agpsType,
                             LocApnTypeMask apnTypeMask){

    LOC_LOGI("GnssAdapter::requestATL handle=%d agpsType=0x%X apnTypeMask=0x%X",
        connHandle, agpsType, apnTypeMask);

    sendMsg( new AgpsMsgRequestATL(
             &mAgpsManager, connHandle, (AGpsExtType)agpsType,
             apnTypeMask));

    return true;
}

/* GnssAdapter::releaseATL
 * Method triggered in QMI thread as part of handling below message:
 * eQMI_LOC_SERVER_REQUEST_CLOSE_V02
 * Triggers teardown of an existing AGPS call */
bool GnssAdapter::releaseATL(int connHandle){

    LOC_LOGI("GnssAdapter::releaseATL");

    /* Release SUPL/INTERNET/SUPL_ES ATL */
    struct AgpsMsgReleaseATL: public LocMsg {

        AgpsManager* mAgpsManager;
        int mConnHandle;

        inline AgpsMsgReleaseATL(AgpsManager* agpsManager, int connHandle) :
                LocMsg(), mAgpsManager(agpsManager), mConnHandle(connHandle) {

            LOC_LOGV("AgpsMsgReleaseATL");
        }

        inline virtual void proc() const {

            LOC_LOGV("AgpsMsgReleaseATL::proc()");
            mAgpsManager->releaseATL(mConnHandle);
        }
    };

    sendMsg( new AgpsMsgReleaseATL(&mAgpsManager, connHandle));

    return true;
}

void GnssAdapter::dataConnOpenCommand(
        AGpsExtType agpsType,
        const char* apnName, int apnLen, AGpsBearerType bearerType){

    LOC_LOGI("GnssAdapter::frameworkDataConnOpen");

    struct AgpsMsgAtlOpenSuccess: public LocMsg {

        AgpsManager* mAgpsManager;
        AGpsExtType mAgpsType;
        char* mApnName;
        int mApnLen;
        AGpsBearerType mBearerType;

        inline AgpsMsgAtlOpenSuccess(AgpsManager* agpsManager, AGpsExtType agpsType,
                const char* apnName, int apnLen, AGpsBearerType bearerType) :
                LocMsg(), mAgpsManager(agpsManager), mAgpsType(agpsType), mApnName(
                        new char[apnLen + 1]), mApnLen(apnLen), mBearerType(bearerType) {

            LOC_LOGV("AgpsMsgAtlOpenSuccess");
            if (mApnName == nullptr) {
                LOC_LOGE("%s] new allocation failed, fatal error.", __func__);
                // Reporting the failure here
                mAgpsManager->reportAtlClosed(mAgpsType);
                return;
            }
            memcpy(mApnName, apnName, apnLen);
            mApnName[apnLen] = 0;
        }

        inline ~AgpsMsgAtlOpenSuccess() {
            delete[] mApnName;
        }

        inline virtual void proc() const {

            LOC_LOGV("AgpsMsgAtlOpenSuccess::proc()");
            mAgpsManager->reportAtlOpenSuccess(mAgpsType, mApnName, mApnLen, mBearerType);
        }
    };
    // Added inital length checks for apnlen check to avoid security issues
    // In case of failure reporting the same
    if (NULL == apnName || apnLen <= 0 || apnLen > MAX_APN_LEN ||
            (strlen(apnName) != (unsigned)apnLen)) {
        LOC_LOGe("%s]: incorrect apnlen length or incorrect apnName", __func__);
        mAgpsManager.reportAtlClosed(agpsType);
    } else {
        sendMsg( new AgpsMsgAtlOpenSuccess(
                    &mAgpsManager, agpsType, apnName, apnLen, bearerType));
    }
}

void GnssAdapter::dataConnClosedCommand(AGpsExtType agpsType){

    LOC_LOGI("GnssAdapter::frameworkDataConnClosed");

    struct AgpsMsgAtlClosed: public LocMsg {

        AgpsManager* mAgpsManager;
        AGpsExtType mAgpsType;

        inline AgpsMsgAtlClosed(AgpsManager* agpsManager, AGpsExtType agpsType) :
                LocMsg(), mAgpsManager(agpsManager), mAgpsType(agpsType) {

            LOC_LOGV("AgpsMsgAtlClosed");
        }

        inline virtual void proc() const {

            LOC_LOGV("AgpsMsgAtlClosed::proc()");
            mAgpsManager->reportAtlClosed(mAgpsType);
        }
    };

    sendMsg( new AgpsMsgAtlClosed(&mAgpsManager, (AGpsExtType)agpsType));
}

void GnssAdapter::dataConnFailedCommand(AGpsExtType agpsType){

    LOC_LOGI("GnssAdapter::frameworkDataConnFailed");

    struct AgpsMsgAtlOpenFailed: public LocMsg {

        AgpsManager* mAgpsManager;
        AGpsExtType mAgpsType;

        inline AgpsMsgAtlOpenFailed(AgpsManager* agpsManager, AGpsExtType agpsType) :
                LocMsg(), mAgpsManager(agpsManager), mAgpsType(agpsType) {

            LOC_LOGV("AgpsMsgAtlOpenFailed");
        }

        inline virtual void proc() const {

            LOC_LOGV("AgpsMsgAtlOpenFailed::proc()");
            mAgpsManager->reportAtlOpenFailed(mAgpsType);
        }
    };

    sendMsg( new AgpsMsgAtlOpenFailed(&mAgpsManager, (AGpsExtType)agpsType));
}

void GnssAdapter::convertSatelliteInfo(std::vector<GnssDebugSatelliteInfo>& out,
                                       const GnssSvType& in_constellation,
                                       const SystemStatusReports& in)
{
    uint64_t sv_mask = 0ULL;
    uint32_t svid_min = 0;
    uint32_t svid_num = 0;
    uint32_t svid_idx = 0;

    uint64_t eph_health_good_mask = 0ULL;
    uint64_t eph_health_bad_mask = 0ULL;
    uint64_t server_perdiction_available_mask = 0ULL;
    float server_perdiction_age = 0.0f;

    // set constellationi based parameters
    switch (in_constellation) {
        case GNSS_SV_TYPE_GPS:
            svid_min = GNSS_BUGREPORT_GPS_MIN;
            svid_num = GPS_NUM;
            svid_idx = 0;
            if (!in.mSvHealth.empty()) {
                eph_health_good_mask = in.mSvHealth.back().mGpsGoodMask;
                eph_health_bad_mask  = in.mSvHealth.back().mGpsBadMask;
            }
            if (!in.mXtra.empty()) {
                server_perdiction_available_mask = in.mXtra.back().mGpsXtraValid;
                server_perdiction_age = (float)(in.mXtra.back().mGpsXtraAge);
            }
            break;
        case GNSS_SV_TYPE_GLONASS:
            svid_min = GNSS_BUGREPORT_GLO_MIN;
            svid_num = GLO_NUM;
            svid_idx = GPS_NUM;
            if (!in.mSvHealth.empty()) {
                eph_health_good_mask = in.mSvHealth.back().mGloGoodMask;
                eph_health_bad_mask  = in.mSvHealth.back().mGloBadMask;
            }
            if (!in.mXtra.empty()) {
                server_perdiction_available_mask = in.mXtra.back().mGloXtraValid;
                server_perdiction_age = (float)(in.mXtra.back().mGloXtraAge);
            }
            break;
        case GNSS_SV_TYPE_QZSS:
            svid_min = GNSS_BUGREPORT_QZSS_MIN;
            svid_num = QZSS_NUM;
            svid_idx = GPS_NUM+GLO_NUM+BDS_NUM+GAL_NUM;
            if (!in.mSvHealth.empty()) {
                eph_health_good_mask = in.mSvHealth.back().mQzssGoodMask;
                eph_health_bad_mask  = in.mSvHealth.back().mQzssBadMask;
            }
            if (!in.mXtra.empty()) {
                server_perdiction_available_mask = in.mXtra.back().mQzssXtraValid;
                server_perdiction_age = (float)(in.mXtra.back().mQzssXtraAge);
            }
            break;
        case GNSS_SV_TYPE_BEIDOU:
            svid_min = GNSS_BUGREPORT_BDS_MIN;
            svid_num = BDS_NUM;
            svid_idx = GPS_NUM+GLO_NUM;
            if (!in.mSvHealth.empty()) {
                eph_health_good_mask = in.mSvHealth.back().mBdsGoodMask;
                eph_health_bad_mask  = in.mSvHealth.back().mBdsBadMask;
            }
            if (!in.mXtra.empty()) {
                server_perdiction_available_mask = in.mXtra.back().mBdsXtraValid;
                server_perdiction_age = (float)(in.mXtra.back().mBdsXtraAge);
            }
            break;
        case GNSS_SV_TYPE_GALILEO:
            svid_min = GNSS_BUGREPORT_GAL_MIN;
            svid_num = GAL_NUM;
            svid_idx = GPS_NUM+GLO_NUM+BDS_NUM;
            if (!in.mSvHealth.empty()) {
                eph_health_good_mask = in.mSvHealth.back().mGalGoodMask;
                eph_health_bad_mask  = in.mSvHealth.back().mGalBadMask;
            }
            if (!in.mXtra.empty()) {
                server_perdiction_available_mask = in.mXtra.back().mGalXtraValid;
                server_perdiction_age = (float)(in.mXtra.back().mGalXtraAge);
            }
            break;
        case GNSS_SV_TYPE_NAVIC:
            svid_min = GNSS_BUGREPORT_NAVIC_MIN;
            svid_num = NAVIC_NUM;
            svid_idx = GPS_NUM+GLO_NUM+QZSS_NUM+BDS_NUM+GAL_NUM;
            if (!in.mSvHealth.empty()) {
                eph_health_good_mask = in.mSvHealth.back().mNavicGoodMask;
                eph_health_bad_mask  = in.mSvHealth.back().mNavicBadMask;
            }
            if (!in.mXtra.empty()) {
                server_perdiction_available_mask = in.mXtra.back().mNavicXtraValid;
                server_perdiction_age = (float)(in.mXtra.back().mNavicXtraAge);
            }
            break;
        default:
            return;
    }

    // extract each sv info from systemstatus report
    for(uint32_t i=0; i<svid_num && (svid_idx+i)<SV_ALL_NUM; i++) {

        GnssDebugSatelliteInfo s = {};
        s.size = sizeof(s);
        s.svid = i + svid_min;
        s.constellation = in_constellation;

        if (!in.mNavData.empty()) {
            s.mEphemerisType   = in.mNavData.back().mNav[svid_idx+i].mType;
            s.mEphemerisSource = in.mNavData.back().mNav[svid_idx+i].mSource;
        }
        else {
            s.mEphemerisType   = GNSS_EPH_TYPE_UNKNOWN;
            s.mEphemerisSource = GNSS_EPH_SOURCE_UNKNOWN;
        }

        sv_mask = 0x1ULL << i;
        if (eph_health_good_mask & sv_mask) {
            s.mEphemerisHealth = GNSS_EPH_HEALTH_GOOD;
        }
        else if (eph_health_bad_mask & sv_mask) {
            s.mEphemerisHealth = GNSS_EPH_HEALTH_BAD;
        }
        else {
            s.mEphemerisHealth = GNSS_EPH_HEALTH_UNKNOWN;
        }

        if (!in.mNavData.empty()) {
            s.ephemerisAgeSeconds =
                (float)(in.mNavData.back().mNav[svid_idx+i].mAgeSec);
        }
        else {
            s.ephemerisAgeSeconds = 0.0f;
        }

        if (server_perdiction_available_mask & sv_mask) {
            s.serverPredictionIsAvailable = true;
        }
        else {
            s.serverPredictionIsAvailable = false;
        }

        s.serverPredictionAgeSeconds = server_perdiction_age;
        out.push_back(s);
    }

    return;
}

bool GnssAdapter::getDebugReport(GnssDebugReport& r)
{
    LOC_LOGD("%s]: ", __func__);

    SystemStatus* systemstatus = getSystemStatus();
    if (nullptr == systemstatus) {
        return false;
    }

    SystemStatusReports reports = {};
    systemstatus->getReport(reports, true);

    r.size = sizeof(r);

    // location block
    r.mLocation.size = sizeof(r.mLocation);
    if(!reports.mLocation.empty() && reports.mLocation.back().mValid) {
        r.mLocation.mValid = true;
        r.mLocation.mLocation.latitude =
            reports.mLocation.back().mLocation.gpsLocation.latitude;
        r.mLocation.mLocation.longitude =
            reports.mLocation.back().mLocation.gpsLocation.longitude;
        r.mLocation.mLocation.altitude =
            reports.mLocation.back().mLocation.gpsLocation.altitude;
        r.mLocation.mLocation.speed =
            (double)(reports.mLocation.back().mLocation.gpsLocation.speed);
        r.mLocation.mLocation.bearing =
            (double)(reports.mLocation.back().mLocation.gpsLocation.bearing);
        r.mLocation.mLocation.accuracy =
            (double)(reports.mLocation.back().mLocation.gpsLocation.accuracy);

        r.mLocation.verticalAccuracyMeters =
            reports.mLocation.back().mLocationEx.vert_unc;
        r.mLocation.speedAccuracyMetersPerSecond =
            reports.mLocation.back().mLocationEx.speed_unc;
        r.mLocation.bearingAccuracyDegrees =
            reports.mLocation.back().mLocationEx.bearing_unc;

        r.mLocation.mUtcReported =
            reports.mLocation.back().mUtcReported;
    }
    else if(!reports.mBestPosition.empty() && reports.mBestPosition.back().mValid) {
        r.mLocation.mValid = true;
        r.mLocation.mLocation.latitude =
                (double)(reports.mBestPosition.back().mBestLat) * RAD2DEG;
        r.mLocation.mLocation.longitude =
                (double)(reports.mBestPosition.back().mBestLon) * RAD2DEG;
        r.mLocation.mLocation.altitude = reports.mBestPosition.back().mBestAlt;
        r.mLocation.mLocation.accuracy =
                (double)(reports.mBestPosition.back().mBestHepe);

        r.mLocation.mUtcReported = reports.mBestPosition.back().mUtcReported;
    }
    else {
        r.mLocation.mValid = false;
    }

    if (r.mLocation.mValid) {
        LOC_LOGV("getDebugReport - lat=%f lon=%f alt=%f speed=%f",
            r.mLocation.mLocation.latitude,
            r.mLocation.mLocation.longitude,
            r.mLocation.mLocation.altitude,
            r.mLocation.mLocation.speed);
    }

    // time block
    r.mTime.size = sizeof(r.mTime);
    if(!reports.mTimeAndClock.empty() && reports.mTimeAndClock.back().mTimeValid) {
        r.mTime.mValid = true;
        r.mTime.timeEstimate =
            (((int64_t)(reports.mTimeAndClock.back().mGpsWeek)*7 +
                        GNSS_UTC_TIME_OFFSET)*24*60*60 -
              (int64_t)(reports.mTimeAndClock.back().mLeapSeconds))*1000ULL +
              (int64_t)(reports.mTimeAndClock.back().mGpsTowMs);

        if (reports.mTimeAndClock.back().mTimeUncNs > 0) {
            // TimeUncNs value is available
            r.mTime.timeUncertaintyNs =
                    (float)(reports.mTimeAndClock.back().mLeapSecUnc)*1000.0f +
                    (float)(reports.mTimeAndClock.back().mTimeUncNs);
        } else {
            // fall back to legacy TimeUnc
            r.mTime.timeUncertaintyNs =
                    ((float)(reports.mTimeAndClock.back().mTimeUnc) +
                     (float)(reports.mTimeAndClock.back().mLeapSecUnc))*1000.0f;
        }

        r.mTime.frequencyUncertaintyNsPerSec =
            (float)(reports.mTimeAndClock.back().mClockFreqBiasUnc);
        LOC_LOGV("getDebugReport - timeestimate=%" PRIu64 " unc=%f frequnc=%f",
                r.mTime.timeEstimate,
                r.mTime.timeUncertaintyNs, r.mTime.frequencyUncertaintyNsPerSec);
    }
    else {
        r.mTime.mValid = false;
    }

    // satellite info block
    convertSatelliteInfo(r.mSatelliteInfo, GNSS_SV_TYPE_GPS, reports);
    convertSatelliteInfo(r.mSatelliteInfo, GNSS_SV_TYPE_GLONASS, reports);
    convertSatelliteInfo(r.mSatelliteInfo, GNSS_SV_TYPE_QZSS, reports);
    convertSatelliteInfo(r.mSatelliteInfo, GNSS_SV_TYPE_BEIDOU, reports);
    convertSatelliteInfo(r.mSatelliteInfo, GNSS_SV_TYPE_GALILEO, reports);
    convertSatelliteInfo(r.mSatelliteInfo, GNSS_SV_TYPE_NAVIC, reports);
    LOC_LOGV("getDebugReport - satellite=%zu", r.mSatelliteInfo.size());

    return true;
}

/* get AGC information from system status and fill it */
void
GnssAdapter::getAgcInformation(GnssMeasurementsNotification& measurements, int msInWeek)
{
    SystemStatus* systemstatus = getSystemStatus();

    if (nullptr != systemstatus) {
        SystemStatusReports reports = {};
        systemstatus->getReport(reports, true);

        if ((!reports.mRfAndParams.empty()) && (!reports.mTimeAndClock.empty()) &&
            (abs(msInWeek - (int)reports.mTimeAndClock.back().mGpsTowMs) < 2000)) {

            for (size_t i = 0; i < measurements.count; i++) {
                switch (measurements.measurements[i].svType) {
                case GNSS_SV_TYPE_GPS:
                case GNSS_SV_TYPE_QZSS:
                    measurements.measurements[i].agcLevelDb =
                            reports.mRfAndParams.back().mAgcGps;
                    measurements.measurements[i].flags |=
                            GNSS_MEASUREMENTS_DATA_AUTOMATIC_GAIN_CONTROL_BIT;
                    break;

                case GNSS_SV_TYPE_GALILEO:
                    measurements.measurements[i].agcLevelDb =
                            reports.mRfAndParams.back().mAgcGal;
                    measurements.measurements[i].flags |=
                            GNSS_MEASUREMENTS_DATA_AUTOMATIC_GAIN_CONTROL_BIT;
                    break;

                case GNSS_SV_TYPE_GLONASS:
                    measurements.measurements[i].agcLevelDb =
                            reports.mRfAndParams.back().mAgcGlo;
                    measurements.measurements[i].flags |=
                            GNSS_MEASUREMENTS_DATA_AUTOMATIC_GAIN_CONTROL_BIT;
                    break;

                case GNSS_SV_TYPE_BEIDOU:
                    measurements.measurements[i].agcLevelDb =
                            reports.mRfAndParams.back().mAgcBds;
                    measurements.measurements[i].flags |=
                            GNSS_MEASUREMENTS_DATA_AUTOMATIC_GAIN_CONTROL_BIT;
                    break;

                case GNSS_SV_TYPE_SBAS:
                case GNSS_SV_TYPE_UNKNOWN:
                default:
                    break;
                }
            }
        }
    }
}

/* get Data information from system status and fill it */
void
GnssAdapter::getDataInformation(GnssDataNotification& data, int msInWeek)
{
    SystemStatus* systemstatus = getSystemStatus();

    LOC_LOGV("%s]: msInWeek=%d", __func__, msInWeek);
    if (nullptr != systemstatus) {
        SystemStatusReports reports = {};
        systemstatus->getReport(reports, true);

        if ((!reports.mRfAndParams.empty()) && (!reports.mTimeAndClock.empty()) &&
            (abs(msInWeek - (int)reports.mTimeAndClock.back().mGpsTowMs) < 2000)) {

            for (int sig = GNSS_LOC_SIGNAL_TYPE_GPS_L1CA;
                 sig < GNSS_LOC_MAX_NUMBER_OF_SIGNAL_TYPES; sig++) {
                data.gnssDataMask[sig] = 0;
                data.jammerInd[sig] = 0.0;
                data.agc[sig] = 0.0;
            }
            if (GNSS_INVALID_JAMMER_IND != reports.mRfAndParams.back().mAgcGps) {
                data.gnssDataMask[GNSS_LOC_SIGNAL_TYPE_GPS_L1CA] |=
                        GNSS_LOC_DATA_AGC_BIT;
                data.agc[GNSS_LOC_SIGNAL_TYPE_GPS_L1CA] =
                        reports.mRfAndParams.back().mAgcGps;
                data.gnssDataMask[GNSS_LOC_SIGNAL_TYPE_QZSS_L1CA] |=
                        GNSS_LOC_DATA_AGC_BIT;
                data.agc[GNSS_LOC_SIGNAL_TYPE_QZSS_L1CA] =
                        reports.mRfAndParams.back().mAgcGps;
                data.gnssDataMask[GNSS_LOC_SIGNAL_TYPE_SBAS_L1_CA] |=
                        GNSS_LOC_DATA_AGC_BIT;
                data.agc[GNSS_LOC_SIGNAL_TYPE_SBAS_L1_CA] =
                    reports.mRfAndParams.back().mAgcGps;
            }
            if (GNSS_INVALID_JAMMER_IND != reports.mRfAndParams.back().mJammerGps) {
                data.gnssDataMask[GNSS_LOC_SIGNAL_TYPE_GPS_L1CA] |=
                        GNSS_LOC_DATA_JAMMER_IND_BIT;
                data.jammerInd[GNSS_LOC_SIGNAL_TYPE_GPS_L1CA] =
                        (double)reports.mRfAndParams.back().mJammerGps;
                data.gnssDataMask[GNSS_LOC_SIGNAL_TYPE_QZSS_L1CA] |=
                        GNSS_LOC_DATA_JAMMER_IND_BIT;
                data.jammerInd[GNSS_LOC_SIGNAL_TYPE_QZSS_L1CA] =
                        (double)reports.mRfAndParams.back().mJammerGps;
                data.gnssDataMask[GNSS_LOC_SIGNAL_TYPE_SBAS_L1_CA] |=
                        GNSS_LOC_DATA_JAMMER_IND_BIT;
                data.jammerInd[GNSS_LOC_SIGNAL_TYPE_SBAS_L1_CA] =
                    (double)reports.mRfAndParams.back().mJammerGps;
            }
            if (GNSS_INVALID_JAMMER_IND != reports.mRfAndParams.back().mAgcGlo) {
                data.gnssDataMask[GNSS_LOC_SIGNAL_TYPE_GLONASS_G1] |=
                        GNSS_LOC_DATA_AGC_BIT;
                data.agc[GNSS_LOC_SIGNAL_TYPE_GLONASS_G1] =
                        reports.mRfAndParams.back().mAgcGlo;
            }
            if (GNSS_INVALID_JAMMER_IND != reports.mRfAndParams.back().mJammerGlo) {
                data.gnssDataMask[GNSS_LOC_SIGNAL_TYPE_GLONASS_G1] |=
                        GNSS_LOC_DATA_JAMMER_IND_BIT;
                data.jammerInd[GNSS_LOC_SIGNAL_TYPE_GLONASS_G1] =
                        (double)reports.mRfAndParams.back().mJammerGlo;
            }
            if (GNSS_INVALID_JAMMER_IND != reports.mRfAndParams.back().mAgcBds) {
                data.gnssDataMask[GNSS_LOC_SIGNAL_TYPE_BEIDOU_B1_I] |=
                        GNSS_LOC_DATA_AGC_BIT;
                data.agc[GNSS_LOC_SIGNAL_TYPE_BEIDOU_B1_I] =
                        reports.mRfAndParams.back().mAgcBds;
            }
            if (GNSS_INVALID_JAMMER_IND != reports.mRfAndParams.back().mJammerBds) {
                data.gnssDataMask[GNSS_LOC_SIGNAL_TYPE_BEIDOU_B1_I] |=
                        GNSS_LOC_DATA_JAMMER_IND_BIT;
                data.jammerInd[GNSS_LOC_SIGNAL_TYPE_BEIDOU_B1_I] =
                        (double)reports.mRfAndParams.back().mJammerBds;
            }
            if (GNSS_INVALID_JAMMER_IND != reports.mRfAndParams.back().mAgcGal) {
                data.gnssDataMask[GNSS_LOC_SIGNAL_TYPE_GALILEO_E1_C] |=
                        GNSS_LOC_DATA_AGC_BIT;
                data.agc[GNSS_LOC_SIGNAL_TYPE_GALILEO_E1_C] =
                        reports.mRfAndParams.back().mAgcGal;
            }
            if (GNSS_INVALID_JAMMER_IND != reports.mRfAndParams.back().mJammerGal) {
                data.gnssDataMask[GNSS_LOC_SIGNAL_TYPE_GALILEO_E1_C] |=
                        GNSS_LOC_DATA_JAMMER_IND_BIT;
                data.jammerInd[GNSS_LOC_SIGNAL_TYPE_GALILEO_E1_C] =
                        (double)reports.mRfAndParams.back().mJammerGal;
            }
        }
    }
}

/* Callbacks registered with loc_net_iface library */
static void agpsOpenResultCb (bool isSuccess, AGpsExtType agpsType, const char* apn,
        AGpsBearerType bearerType, void* userDataPtr) {
    LOC_LOGD("%s]: ", __func__);
    if (userDataPtr == nullptr) {
        LOC_LOGE("%s]: userDataPtr is nullptr.", __func__);
        return;
    }
    if (apn == nullptr) {
        LOC_LOGE("%s]: apn is nullptr.", __func__);
        return;
    }
    GnssAdapter* adapter = (GnssAdapter*)userDataPtr;
    if (isSuccess) {
        adapter->dataConnOpenCommand(agpsType, apn, strlen(apn), bearerType);
    } else {
        adapter->dataConnFailedCommand(agpsType);
    }
}

static void agpsCloseResultCb (bool isSuccess, AGpsExtType agpsType, void* userDataPtr) {
    LOC_LOGD("%s]: ", __func__);
    if (userDataPtr == nullptr) {
        LOC_LOGE("%s]: userDataPtr is nullptr.", __func__);
        return;
    }
    GnssAdapter* adapter = (GnssAdapter*)userDataPtr;
    if (isSuccess) {
        adapter->dataConnClosedCommand(agpsType);
    } else {
        adapter->dataConnFailedCommand(agpsType);
    }
}

void
GnssAdapter::saveGnssEnergyConsumedCallback(GnssEnergyConsumedCallback energyConsumedCb) {
    mGnssEnergyConsumedCb = energyConsumedCb;
}

void
GnssAdapter::getGnssEnergyConsumedCommand(GnssEnergyConsumedCallback energyConsumedCb) {
    struct MsgGetGnssEnergyConsumed : public LocMsg {
        GnssAdapter& mAdapter;
        LocApiBase& mApi;
        GnssEnergyConsumedCallback mEnergyConsumedCb;
        inline MsgGetGnssEnergyConsumed(GnssAdapter& adapter, LocApiBase& api,
                                        GnssEnergyConsumedCallback energyConsumedCb) :
            LocMsg(),
            mAdapter(adapter),
            mApi(api),
            mEnergyConsumedCb(energyConsumedCb){}
        inline virtual void proc() const {
            mAdapter.saveGnssEnergyConsumedCallback(mEnergyConsumedCb);
            mApi.getGnssEnergyConsumed();
        }
    };

    sendMsg(new MsgGetGnssEnergyConsumed(*this, *mLocApi, energyConsumedCb));
}

// Set tunc constrained mode, use 0 session id to indicate
// that no callback is needed. Session id 0 is used for calls that
// are not invoked from the integration api, e.g.: initial configuration
// from the configure file
void
GnssAdapter::setConstrainedTunc(bool enable, float tuncConstraint,
                                uint32_t energyBudget, uint32_t sessionId) {

    mLocConfigInfo.tuncConfigInfo.isValid = true;
    mLocConfigInfo.tuncConfigInfo.enable = enable;
    mLocConfigInfo.tuncConfigInfo.tuncThresholdMs = tuncConstraint;
    mLocConfigInfo.tuncConfigInfo.energyBudget = energyBudget;

    LocApiResponse* locApiResponse = nullptr;
    if (sessionId != 0) {
        locApiResponse =
                new LocApiResponse(*getContext(),
                                   [this, sessionId] (LocationError err) {
                                    reportResponse(err, sessionId);});
        if (!locApiResponse) {
            LOC_LOGe("memory alloc failed");
        }
    }
    mLocApi->setConstrainedTuncMode(
            enable, tuncConstraint, energyBudget, locApiResponse);
}

uint32_t
GnssAdapter::setConstrainedTuncCommand (bool enable, float tuncConstraint,
                                        uint32_t energyBudget) {
    // generated session id will be none-zero
    uint32_t sessionId = generateSessionId();
    LOC_LOGd("session id %u", sessionId);

    struct MsgEnableTUNC : public LocMsg {
        GnssAdapter& mAdapter;
        uint32_t mSessionId;
        bool mEnable;
        float mTuncConstraint;
        uint32_t mEnergyBudget;

        inline MsgEnableTUNC(GnssAdapter& adapter,
                             uint32_t sessionId,
                             bool enable,
                             float tuncConstraint,
                             uint32_t energyBudget) :
            LocMsg(),
            mAdapter(adapter),
            mSessionId(sessionId),
            mEnable(enable),
            mTuncConstraint(tuncConstraint),
            mEnergyBudget(energyBudget) {}
        inline virtual void proc() const {
            mAdapter.setConstrainedTunc(mEnable, mTuncConstraint,
                                        mEnergyBudget, mSessionId);
        }
    };

    sendMsg(new MsgEnableTUNC(*this, sessionId, enable,
                              tuncConstraint, energyBudget));

    return sessionId;
}

// Set position assisted clock estimator, use 0 session id to indicate
// that no callback is needed. Session id 0 is used for calls that are
// not invoked from the integration api, e.g.: initial configuration
// from the configure file.
void
GnssAdapter::setPositionAssistedClockEstimator(bool enable,
                                               uint32_t sessionId) {

    mLocConfigInfo.paceConfigInfo.isValid = true;
    mLocConfigInfo.paceConfigInfo.enable = enable;
    LocApiResponse* locApiResponse = nullptr;
    if (sessionId != 0) {
        locApiResponse =
                new LocApiResponse(*getContext(),
                                   [this, sessionId] (LocationError err) {
                                   reportResponse(err, sessionId);});
        if (!locApiResponse) {
            LOC_LOGe("memory alloc failed");
        }
    }
    mLocApi->setPositionAssistedClockEstimatorMode(enable, locApiResponse);
}

uint32_t
GnssAdapter::setPositionAssistedClockEstimatorCommand(bool enable) {
    // generated session id will be none-zero
    uint32_t sessionId = generateSessionId();
    LOC_LOGd("session id %u", sessionId);

    struct MsgEnablePACE : public LocMsg {
        GnssAdapter& mAdapter;
        uint32_t mSessionId;
        bool mEnable;
        inline MsgEnablePACE(GnssAdapter& adapter,
                             uint32_t sessionId, bool enable) :
            LocMsg(),
            mAdapter(adapter),
            mSessionId(sessionId),
            mEnable(enable){}
        inline virtual void proc() const {
            mAdapter.setPositionAssistedClockEstimator(mEnable, mSessionId);
        }
    };

    sendMsg(new MsgEnablePACE(*this, sessionId, enable));
    return sessionId;
}

void GnssAdapter::gnssUpdateSvConfig(
        uint32_t sessionId, const GnssSvTypeConfig& constellationEnablementConfig,
        const GnssSvIdConfig&   blacklistSvConfig) {

    // suspend all tracking sessions to apply the constellation config
    suspendSessions();
    if (constellationEnablementConfig.size == sizeof(constellationEnablementConfig)) {
        // check whether if any constellation is removed from the new config
        GnssSvTypesMask currentEnabledMask = mGnssSvTypeConfig.enabledSvTypesMask;
        GnssSvTypesMask newEnabledMask = constellationEnablementConfig.enabledSvTypesMask;
        GnssSvTypesMask enabledRemoved = currentEnabledMask & (currentEnabledMask ^ newEnabledMask);
        // Send reset if any constellation is removed from the enabled list
        if (enabledRemoved != 0) {
            mLocApi->resetConstellationControl();
        }

        // if the constellation config is valid, issue request to modem
        // to enable/disable constellation
        mLocApi->setConstellationControl(mGnssSvTypeConfig);
    } else if (constellationEnablementConfig.size == 0) {
        // when the size is not set, meaning reset to modem default
        mLocApi->resetConstellationControl();
    }
    // save the constellation settings to be used for modem SSR
    mGnssSvTypeConfig = constellationEnablementConfig;

    // handle blacklisted SV settings
    mGnssSvIdConfig   = blacklistSvConfig;
    // process blacklist svs info
    mBlacklistedSvIds.clear();
    // need to save the balcklisted sv info into mBlacklistedSvIds as well
    convertFromGnssSvIdConfig(blacklistSvConfig, mBlacklistedSvIds);
    LocApiResponse* locApiResponse = new LocApiResponse(*getContext(),
            [this, sessionId] (LocationError err) {
            reportResponse(err, sessionId);});
    if (!locApiResponse) {
        LOC_LOGe("memory alloc failed");
    }
    mLocApi->setBlacklistSv(mGnssSvIdConfig, locApiResponse);

    // resume all tracking sessions after the constellation config has been applied
    restartSessions(false);
}

uint32_t
GnssAdapter::gnssUpdateSvConfigCommand(
        const GnssSvTypeConfig& constellationEnablementConfig,
        const GnssSvIdConfig& blacklistSvConfig) {

    // generated session id will be none-zero
    uint32_t sessionId = generateSessionId();
    LOC_LOGd("session id %u", sessionId);

    struct MsgUpdateSvConfig : public LocMsg {
        GnssAdapter&     mAdapter;
        uint32_t         mSessionId;
        GnssSvTypeConfig mConstellationEnablementConfig;
        GnssSvIdConfig   mBlacklistSvIdConfig;

        inline MsgUpdateSvConfig(GnssAdapter& adapter,
                                 uint32_t sessionId,
                                 const GnssSvTypeConfig& constellationEnablementConfig,
                                 const GnssSvIdConfig& blacklistSvConfig) :
            LocMsg(),
            mAdapter(adapter),
            mSessionId(sessionId),
            mConstellationEnablementConfig(constellationEnablementConfig),
            mBlacklistSvIdConfig(blacklistSvConfig) {}
        inline virtual void proc() const {
            mAdapter.gnssUpdateSvConfig(mSessionId, mConstellationEnablementConfig,
                                        mBlacklistSvIdConfig);
        }
    };

    if (sessionId != 0) {
        sendMsg(new MsgUpdateSvConfig(*this, sessionId, constellationEnablementConfig,
                                      blacklistSvConfig));
    }
    return sessionId;
}

void GnssAdapter::gnssUpdateSecondaryBandConfig(
        uint32_t sessionId, const GnssSvTypeConfig& secondaryBandConfig) {

    LocApiResponse* locApiResponse = new LocApiResponse(*getContext(),
            [this, sessionId] (LocationError err) {
            reportResponse(err, sessionId);});
    if (!locApiResponse) {
        LOC_LOGe("memory alloc failed");
    }

    // handle secondary band info
    mGnssSeconaryBandConfig = secondaryBandConfig;
    gnssSecondaryBandConfigUpdate(locApiResponse);
}

uint32_t
GnssAdapter::gnssUpdateSecondaryBandConfigCommand(
        const GnssSvTypeConfig& secondaryBandConfig) {

    // generated session id will be none-zero
    uint32_t sessionId = generateSessionId();
    LOC_LOGd("session id %u", sessionId);

    struct MsgUpdateSecondaryBandConfig : public LocMsg {
        GnssAdapter&     mAdapter;
        uint32_t         mSessionId;
        GnssSvTypeConfig mSecondaryBandConfig;

        inline MsgUpdateSecondaryBandConfig(GnssAdapter& adapter,
                                 uint32_t sessionId,
                                 const GnssSvTypeConfig& secondaryBandConfig) :
            LocMsg(),
            mAdapter(adapter),
            mSessionId(sessionId),
            mSecondaryBandConfig(secondaryBandConfig) {}
        inline virtual void proc() const {
            mAdapter.gnssUpdateSecondaryBandConfig(mSessionId,  mSecondaryBandConfig);
        }
    };

    if (sessionId != 0) {
        sendMsg(new MsgUpdateSecondaryBandConfig(*this, sessionId, secondaryBandConfig));
    }
    return sessionId;
}

// This function currently retrieves secondary band configuration
// for constellation enablement/disablement.
void
GnssAdapter::gnssGetSecondaryBandConfig(uint32_t sessionId) {

    LocApiResponse* locApiResponse = new LocApiResponse(*getContext(),
            [this, sessionId] (LocationError err) {
            reportResponse(err, sessionId);});
    if (!locApiResponse) {
        LOC_LOGe("memory alloc failed");
    }

    mLocApi->getConstellationMultiBandConfig(sessionId, locApiResponse);
}

uint32_t
GnssAdapter::gnssGetSecondaryBandConfigCommand() {

    // generated session id will be none-zero
    uint32_t sessionId = generateSessionId();
    LOC_LOGd("session id %u", sessionId);

    struct MsgGetSecondaryBandConfig : public LocMsg {
        GnssAdapter& mAdapter;
        uint32_t     mSessionId;
        inline MsgGetSecondaryBandConfig(GnssAdapter& adapter,
                              uint32_t sessionId) :
            LocMsg(),
            mAdapter(adapter),
            mSessionId(sessionId) {}
        inline virtual void proc() const {
            mAdapter.gnssGetSecondaryBandConfig(mSessionId);
        }
    };

    if (sessionId != 0) {
        sendMsg(new MsgGetSecondaryBandConfig(*this, sessionId));
    }
    return sessionId;
}

void
GnssAdapter::configLeverArm(uint32_t sessionId,
                            const LeverArmConfigInfo& configInfo) {

    LocationError err = LOCATION_ERROR_NOT_SUPPORTED;
    if (true == mEngHubProxy->configLeverArm(configInfo)) {
        err = LOCATION_ERROR_SUCCESS;
    }
    reportResponse(err, sessionId);
}

uint32_t
GnssAdapter::configLeverArmCommand(const LeverArmConfigInfo& configInfo) {

    // generated session id will be none-zero
    uint32_t sessionId = generateSessionId();
    LOC_LOGd("session id %u", sessionId);

    struct MsgConfigLeverArm : public LocMsg {
        GnssAdapter&       mAdapter;
        uint32_t           mSessionId;
        LeverArmConfigInfo mConfigInfo;

        inline MsgConfigLeverArm(GnssAdapter& adapter,
                                 uint32_t sessionId,
                                 const LeverArmConfigInfo& configInfo) :
            LocMsg(),
            mAdapter(adapter),
            mSessionId(sessionId),
            mConfigInfo(configInfo) {}
        inline virtual void proc() const {
            // save the lever ARM config info for translate position from GNSS antenna based
            // to VRP based
            if (mConfigInfo.leverArmValidMask & LEVER_ARM_TYPE_GNSS_TO_VRP_BIT) {
                mAdapter.mLocConfigInfo.leverArmConfigInfo.leverArmValidMask |=
                        LEVER_ARM_TYPE_GNSS_TO_VRP_BIT;
                mAdapter.mLocConfigInfo.leverArmConfigInfo.gnssToVRP = mConfigInfo.gnssToVRP;
            }
            mAdapter.configLeverArm(mSessionId, mConfigInfo);
        }
    };

    sendMsg(new MsgConfigLeverArm(*this, sessionId, configInfo));
    return sessionId;
}

void
GnssAdapter::configRobustLocation(uint32_t sessionId,
                                  bool enable, bool enableForE911) {

    mLocConfigInfo.robustLocationConfigInfo.isValid = true;
    mLocConfigInfo.robustLocationConfigInfo.enable = enable;
    mLocConfigInfo.robustLocationConfigInfo.enableFor911 = enableForE911;

    LocApiResponse* locApiResponse = nullptr;
    if (sessionId != 0) {
        locApiResponse =
                new LocApiResponse(*getContext(),
                                   [this, sessionId] (LocationError err) {
                                   reportResponse(err, sessionId);});
        if (!locApiResponse) {
            LOC_LOGe("memory alloc failed");
        }
    }
    mLocApi->configRobustLocation(enable, enableForE911, locApiResponse);
}

uint32_t GnssAdapter::configRobustLocationCommand(
        bool enable, bool enableForE911) {

    // generated session id will be none-zero
    uint32_t sessionId = generateSessionId();
    LOC_LOGd("session id %u", sessionId);

    struct MsgConfigRobustLocation : public LocMsg {
        GnssAdapter&     mAdapter;
        uint32_t         mSessionId;
        bool             mEnable;
        bool             mEnableForE911;

        inline MsgConfigRobustLocation(GnssAdapter& adapter,
                                uint32_t sessionId,
                                bool     enable,
                                bool     enableForE911) :
            LocMsg(),
            mAdapter(adapter),
            mSessionId(sessionId),
            mEnable(enable),
            mEnableForE911(enableForE911) {}
        inline virtual void proc() const {
            mAdapter.configRobustLocation(mSessionId, mEnable, mEnableForE911);
        }
    };

    sendMsg(new MsgConfigRobustLocation(*this, sessionId, enable, enableForE911));
    return sessionId;
}

void
GnssAdapter::configMinGpsWeek(uint32_t sessionId, uint16_t minGpsWeek) {

    // suspend all sessions for modem to take the min GPS week config
    suspendSessions();

    LocApiResponse* locApiResponse = nullptr;
    if (sessionId != 0) {
        locApiResponse =
                new LocApiResponse(*getContext(),
                                   [this, sessionId] (LocationError err) {
                                   reportResponse(err, sessionId);});
        if (!locApiResponse) {
            LOC_LOGe("memory alloc failed");
        }
    }
    mLocApi->configMinGpsWeek(minGpsWeek, locApiResponse);

    // resume all tracking sessions after the min GPS week config
    // has been changed
    restartSessions(false);
}

uint32_t GnssAdapter::configMinGpsWeekCommand(uint16_t minGpsWeek) {
    // generated session id will be none-zero
    uint32_t sessionId = generateSessionId();
    LOC_LOGd("session id %u", sessionId);

    struct MsgConfigMinGpsWeek : public LocMsg {
        GnssAdapter&     mAdapter;
        uint32_t         mSessionId;
        uint16_t         mMinGpsWeek;

        inline MsgConfigMinGpsWeek(GnssAdapter& adapter,
                                   uint32_t sessionId,
                                   uint16_t minGpsWeek) :
            LocMsg(),
            mAdapter(adapter),
            mSessionId(sessionId),
            mMinGpsWeek(minGpsWeek) {}
        inline virtual void proc() const {
            mAdapter.configMinGpsWeek(mSessionId, mMinGpsWeek);
        }
    };

    sendMsg(new MsgConfigMinGpsWeek(*this, sessionId, minGpsWeek));
    return sessionId;
}

uint32_t GnssAdapter::configDeadReckoningEngineParamsCommand(
        const DeadReckoningEngineConfig& dreConfig) {

    // generated session id will be none-zero
    uint32_t sessionId = generateSessionId();
    LOC_LOGd("session id %u", sessionId);

    struct MsgConfigDrEngine : public LocMsg {
        GnssAdapter& mAdapter;
        uint32_t     mSessionId;
        DeadReckoningEngineConfig mDreConfig;

        inline MsgConfigDrEngine(GnssAdapter& adapter,
                                  uint32_t sessionId,
                                  const DeadReckoningEngineConfig& dreConfig) :
            LocMsg(),
            mAdapter(adapter),
            mSessionId(sessionId),
            mDreConfig(dreConfig) {}
        inline virtual void proc() const {
            LocationError err = LOCATION_ERROR_NOT_SUPPORTED;
            if (true == mAdapter.mEngHubProxy->configDeadReckoningEngineParams(mDreConfig)) {
                err = LOCATION_ERROR_SUCCESS;
            }
            mAdapter.reportResponse(err, mSessionId);
        }
    };

    sendMsg(new MsgConfigDrEngine(*this, sessionId, dreConfig));
    return sessionId;
}

void GnssAdapter::reportGnssConfigEvent(uint32_t sessionId, const GnssConfig& gnssConfig)
{
    struct MsgReportGnssConfig : public LocMsg {
        GnssAdapter& mAdapter;
        uint32_t     mSessionId;
        mutable GnssConfig   mGnssConfig;
        inline MsgReportGnssConfig(GnssAdapter& adapter,
                                   uint32_t sessionId,
                                   const GnssConfig& gnssConfig) :
            LocMsg(),
            mAdapter(adapter),
            mSessionId(sessionId),
            mGnssConfig(gnssConfig) {}
        inline virtual void proc() const {
            // Invoke control clients config callback
            if (nullptr != mAdapter.mControlCallbacks.gnssConfigCb) {
                mAdapter.mControlCallbacks.gnssConfigCb(mSessionId, mGnssConfig);
            } else {
                LOC_LOGe("Failed to report, callback not registered");
            }
        }
    };

    sendMsg(new MsgReportGnssConfig(*this, sessionId, gnssConfig));
}

/* ==== Eng Hub Proxy ================================================================= */
/* ======== UTILITIES ================================================================= */
void
GnssAdapter::initEngHubProxyCommand() {
    LOC_LOGD("%s]: ", __func__);

    struct MsgInitEngHubProxy : public LocMsg {
        GnssAdapter* mAdapter;
        inline MsgInitEngHubProxy(GnssAdapter* adapter) :
            LocMsg(),
            mAdapter(adapter) {}
        inline virtual void proc() const {
            mAdapter->initEngHubProxy();
        }
    };

    sendMsg(new MsgInitEngHubProxy(this));
}

bool
GnssAdapter::initEngHubProxy() {
    static bool firstTime = true;
    static bool engHubLoadSuccessful = false;

    const char *error = nullptr;
    unsigned int processListLength = 0;
    loc_process_info_s_type* processInfoList = nullptr;

    do {
        // load eng hub only once
        if (firstTime == false) {
            break;
        }

        int rc = loc_read_process_conf(LOC_PATH_IZAT_CONF, &processListLength,
                                       &processInfoList);
        if (rc != 0) {
            LOC_LOGE("%s]: failed to parse conf file", __func__);
            break;
        }

        bool pluginDaemonEnabled = false;
        // go over the conf table to see whether any plugin daemon is enabled
        for (unsigned int i = 0; i < processListLength; i++) {
            if ((strncmp(processInfoList[i].name[0], PROCESS_NAME_ENGINE_SERVICE,
                         strlen(PROCESS_NAME_ENGINE_SERVICE)) == 0) &&
                (processInfoList[i].proc_status == ENABLED)) {
                pluginDaemonEnabled = true;
                break;
            }
        }

        // no plugin daemon is enabled for this platform,
        // check if external engine is present for which we need
        // libloc_eng_hub.so to be loaded
        if (pluginDaemonEnabled == false) {
            UTIL_READ_CONF(LOC_PATH_IZAT_CONF, izatConfParamTable);
            if (!loadEngHubForExternalEngine) {
                break;
            }
        }

        // load the engine hub .so, if the .so is not present
        // all EngHubProxyBase calls will turn into no-op.
        void *handle = nullptr;
        if ((handle = dlopen("libloc_eng_hub.so", RTLD_NOW)) == nullptr) {
            if ((error = dlerror()) != nullptr) {
                LOC_LOGE("%s]: libloc_eng_hub.so not found %s !", __func__, error);
            }
            break;
        }

        // prepare the callback functions
        // callback function for engine hub to report back position event
        GnssAdapterReportEnginePositionsEventCb reportPositionEventCb =
            [this](int count, EngineLocationInfo* locationArr) {
                    // report from engine hub on behalf of PPE will be treated as fromUlp
                    reportEnginePositionsEvent(count, locationArr);
            };

        // callback function for engine hub to report back sv event
        GnssAdapterReportSvEventCb reportSvEventCb =
            [this](const GnssSvNotification& svNotify, bool fromEngineHub) {
                   reportSvEvent(svNotify, fromEngineHub);
            };

        // callback function for engine hub to request for complete aiding data
        GnssAdapterReqAidingDataCb reqAidingDataCb =
            [this] (const GnssAidingDataSvMask& svDataMask) {
            mLocApi->requestForAidingData(svDataMask);
        };

        GnssAdapterUpdateNHzRequirementCb updateNHzRequirementCb =
            [this] (bool nHzNeeded, bool nHzMeasNeeded) {

            if (nHzMeasNeeded &&
                    (!checkMask(LOC_API_ADAPTER_BIT_GNSS_NHZ_MEASUREMENT))) {
                updateEvtMask(LOC_API_ADAPTER_BIT_GNSS_NHZ_MEASUREMENT,
                    LOC_REGISTRATION_MASK_ENABLED);
            } else if (checkMask(LOC_API_ADAPTER_BIT_GNSS_NHZ_MEASUREMENT)) {
                updateEvtMask(LOC_API_ADAPTER_BIT_GNSS_NHZ_MEASUREMENT,
                    LOC_REGISTRATION_MASK_DISABLED);
            }

            if (mNHzNeeded != nHzNeeded) {
                mNHzNeeded = nHzNeeded;
                checkAndRestartSPESession();
            }
       };

        getEngHubProxyFn* getter = (getEngHubProxyFn*) dlsym(handle, "getEngHubProxy");
        if(getter != nullptr) {
            EngineHubProxyBase* hubProxy = (*getter) (mMsgTask, mSystemStatus->getOsObserver(),
                                                      reportPositionEventCb,
                                                      reportSvEventCb, reqAidingDataCb,
                                                      updateNHzRequirementCb);
            if (hubProxy != nullptr) {
                mEngHubProxy = hubProxy;
                engHubLoadSuccessful = true;
            }
        }
        else {
            LOC_LOGD("%s]: entered, did not find function", __func__);
        }

        LOC_LOGD("%s]: first time initialization %d, returned %d",
                 __func__, firstTime, engHubLoadSuccessful);

    } while (0);

    if (processInfoList != nullptr) {
        free (processInfoList);
        processInfoList = nullptr;
    }

    firstTime = false;
    return engHubLoadSuccessful;
}

/* ==== DGnss Usable Reporter ========================================================= */
/* ======== UTILITIES ================================================================= */

void GnssAdapter::initDGnssUsableReporter()
{
    if (nullptr == mCdfwInterface) {
        void* libHandle = nullptr;
        getCdfwInterface getter  = (getCdfwInterface)dlGetSymFromLib(libHandle,
                          "libcdfw.so", "getQCdfwInterface");
        if (nullptr == getter) {
            LOC_LOGe("dlGetSymFromLib getQCdfwInterface failed");
        } else {
            mCdfwInterface = getter();
        }
    }
    if (nullptr != mCdfwInterface) {
        QDgnssSessionActiveCb qDgnssSessionActiveCb = [this] (bool sessionActive) {
            mDGnssNeedReport = sessionActive;
        };
        mQDgnssListenerHDL = mCdfwInterface->createUsableReporter(qDgnssSessionActiveCb);
    }
}
