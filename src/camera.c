#include "camera.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>

#include "rk_aiq_user_api2_ae.h"
#include "rk_aiq_user_api2_sysctl.h"
#include "rk_comm_venc.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vi.h"

#include "log.h"

#define LOG_TAG "camera"

#define CAM_ID 0    /* rkaiq camera (ISP) index */
#define VI_DEV 0
#define VI_PIPE 0
#define VI_CHN 0     /* rkisp main path: full resolution */
#define VI_CHN_SUB 1 /* rkisp self path: downscaled */
#define VENC_MAIN 0
#define VENC_JPEG 1
#define VENC_SUB 2

static struct {
    cam_config cfg;
    cam_frame_fn on_frame;
    void *user;

    rk_aiq_sys_ctx_t *aiq;
    char sensor[64];
    bool sys_init, vi_dev, vi_chn, venc_main, venc_jpeg, bound;
    bool vi_sub, venc_sub, bound_sub;

    pthread_t thread, sub_thread;
    bool sub_thread_started;
    volatile bool running;

    pthread_mutex_t stats_mu;
    cam_stats stats;
} cam = {.stats_mu = PTHREAD_MUTEX_INITIALIZER};

/* ---- ISP ------------------------------------------------------------------ */

static int isp_start(void) {
    const rk_aiq_working_mode_t mode = RK_AIQ_WORKING_MODE_NORMAL; /* linear, no HDR */
    char hdr[8];
    /* rkaiq reads the working mode from the environment during init. */
    snprintf(hdr, sizeof hdr, "%d", (int)mode);
    setenv("HDR_MODE", hdr, 1);

    rk_aiq_static_info_t info;
    memset(&info, 0, sizeof info);
    if (rk_aiq_uapi2_sysctl_enumStaticMetasByPhyId(CAM_ID, &info) != XCAM_RETURN_NO_ERROR ||
        !info.sensor_info.sensor_name[0]) {
        LOGE("no camera sensor found (are the camera drivers loaded?)");
        return -1;
    }
    snprintf(cam.sensor, sizeof cam.sensor, "%s", info.sensor_info.sensor_name);
    LOGI("sensor %s, IQ files %s", cam.sensor, cam.cfg.iq_dir);

    /* Two raw buffers between sensor and ISP, as rkipc uses. The IQ file's
     * default scene applies. rkaiq prints "XCORE:E:invalid main scene len!"
     * during init on this board whatever the program does; it is harmless. */
    rk_aiq_uapi2_sysctl_preInit_devBufCnt(cam.sensor, "rkraw_rx", 2);

    cam.aiq = rk_aiq_uapi2_sysctl_init(cam.sensor, cam.cfg.iq_dir, NULL, NULL);
    if (!cam.aiq) {
        LOGE("rkaiq init failed (IQ files in %s?)", cam.cfg.iq_dir);
        return -1;
    }
    if (rk_aiq_uapi2_sysctl_prepare(cam.aiq, 0, 0, mode) != XCAM_RETURN_NO_ERROR) {
        LOGE("rkaiq prepare failed");
        goto fail;
    }
    if (rk_aiq_uapi2_sysctl_start(cam.aiq) != XCAM_RETURN_NO_ERROR) {
        LOGE("rkaiq start failed");
        goto fail;
    }

    /* Fix the sensor frame rate; AE would otherwise lower it in the dark. */
    Uapi_ExpSwAttrV2_t ae;
    if (rk_aiq_user_api2_ae_getExpSwAttr(cam.aiq, &ae) == XCAM_RETURN_NO_ERROR) {
        ae.stAuto.stFrmRate.isFpsFix = true;
        ae.stAuto.stFrmRate.FpsValue = (float)cam.cfg.fps;
        if (rk_aiq_user_api2_ae_setExpSwAttr(cam.aiq, ae) != XCAM_RETURN_NO_ERROR)
            LOGW("could not fix the sensor frame rate at %d", cam.cfg.fps);
    }
    return 0;

fail:
    rk_aiq_uapi2_sysctl_deinit(cam.aiq);
    cam.aiq = NULL;
    return -1;
}

static void isp_stop(void) {
    if (!cam.aiq)
        return;
    rk_aiq_uapi2_sysctl_stop(cam.aiq, false);
    rk_aiq_uapi2_sysctl_deinit(cam.aiq);
    cam.aiq = NULL;
}

/* ---- VI ------------------------------------------------------------------- */

static int wrap_lines(void) {
    /* A quarter of the picture, as rkipc does; the hardware wants >= 128. */
    int lines = cam.cfg.height / 4;
    return lines < 128 ? 128 : lines;
}

static int vi_start(void) {
    VI_DEV_ATTR_S dev;
    memset(&dev, 0, sizeof dev);
    RK_S32 ret = RK_MPI_VI_GetDevAttr(VI_DEV, &dev);
    if (ret == RK_ERR_VI_NOT_CONFIG && (ret = RK_MPI_VI_SetDevAttr(VI_DEV, &dev)) != RK_SUCCESS) {
        LOGE("VI SetDevAttr: %#x", ret);
        return -1;
    }
    if (RK_MPI_VI_GetDevIsEnable(VI_DEV) != RK_SUCCESS) {
        if ((ret = RK_MPI_VI_EnableDev(VI_DEV)) != RK_SUCCESS) {
            LOGE("VI EnableDev: %#x", ret);
            return -1;
        }
        cam.vi_dev = true;
        VI_DEV_BIND_PIPE_S bind;
        memset(&bind, 0, sizeof bind);
        bind.u32Num = 1;
        bind.PipeId[0] = VI_PIPE;
        if ((ret = RK_MPI_VI_SetDevBindPipe(VI_DEV, &bind)) != RK_SUCCESS) {
            LOGE("VI SetDevBindPipe: %#x", ret);
            return -1;
        }
    }

    VI_CHN_ATTR_S chn;
    memset(&chn, 0, sizeof chn);
    chn.stIspOpt.u32BufCount = 2;
    chn.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    chn.stIspOpt.stMaxSize.u32Width = cam.cfg.width;
    chn.stIspOpt.stMaxSize.u32Height = cam.cfg.height;
    chn.stSize.u32Width = cam.cfg.width;
    chn.stSize.u32Height = cam.cfg.height;
    chn.enPixelFormat = RK_FMT_YUV420SP;
    chn.enCompressMode = COMPRESS_MODE_NONE;
    chn.u32Depth = 0; /* bound to VENC; the program never takes raw frames */
    if ((ret = RK_MPI_VI_SetChnAttr(VI_PIPE, VI_CHN, &chn)) != RK_SUCCESS) {
        LOGE("VI SetChnAttr %dx%d: %#x", cam.cfg.width, cam.cfg.height, ret);
        return -1;
    }
    if (cam.cfg.wrap) {
        VI_CHN_BUF_WRAP_S wrap;
        memset(&wrap, 0, sizeof wrap);
        wrap.bEnable = RK_TRUE;
        wrap.u32BufLine = wrap_lines();
        wrap.u32WrapBufferSize = wrap.u32BufLine * cam.cfg.width * 3 / 2; /* NV12 */
        if ((ret = RK_MPI_VI_SetChnWrapBufAttr(VI_PIPE, VI_CHN, &wrap)) != RK_SUCCESS) {
            LOGE("VI SetChnWrapBufAttr: %#x", ret);
            return -1;
        }
    }
    if ((ret = RK_MPI_VI_EnableChn(VI_PIPE, VI_CHN)) != RK_SUCCESS) {
        LOGE("VI EnableChn: %#x", ret);
        return -1;
    }
    cam.vi_chn = true;
    return 0;
}

/* The ISP's self path scales the picture down for the sub stream. Two frame
 * buffers of sub size, no wrap (that is for the full-size main path). */
static int vi_sub_start(void) {
    VI_CHN_ATTR_S chn;
    memset(&chn, 0, sizeof chn);
    chn.stIspOpt.u32BufCount = 2;
    chn.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    chn.stIspOpt.stMaxSize.u32Width = cam.cfg.sub_width;
    chn.stIspOpt.stMaxSize.u32Height = cam.cfg.sub_height;
    chn.stSize.u32Width = cam.cfg.sub_width;
    chn.stSize.u32Height = cam.cfg.sub_height;
    chn.enPixelFormat = RK_FMT_YUV420SP;
    chn.enCompressMode = COMPRESS_MODE_NONE;
    chn.u32Depth = 0;
    RK_S32 ret = RK_MPI_VI_SetChnAttr(VI_PIPE, VI_CHN_SUB, &chn);
    if (ret != RK_SUCCESS) {
        LOGE("VI SetChnAttr (sub) %dx%d: %#x", cam.cfg.sub_width, cam.cfg.sub_height, ret);
        return -1;
    }
    if ((ret = RK_MPI_VI_EnableChn(VI_PIPE, VI_CHN_SUB)) != RK_SUCCESS) {
        LOGE("VI EnableChn (sub): %#x", ret);
        return -1;
    }
    cam.vi_sub = true;
    return 0;
}

/* ---- VENC ----------------------------------------------------------------- */

/* An H.264/H.265 CBR encoder channel; the main stream also gets wrap mode. */
static int venc_video_start(int chn, bool h265, int width, int height, int kbps, bool wrap) {
    const cam_config *c = &cam.cfg;
    VENC_CHN_ATTR_S attr;
    memset(&attr, 0, sizeof attr);
    attr.stVencAttr.enType = h265 ? RK_VIDEO_ID_HEVC : RK_VIDEO_ID_AVC;
    attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    attr.stVencAttr.u32Profile = h265 ? 0 /* Main */ : 100 /* High */;
    attr.stVencAttr.u32MaxPicWidth = width;
    attr.stVencAttr.u32MaxPicHeight = height;
    attr.stVencAttr.u32PicWidth = width;
    attr.stVencAttr.u32PicHeight = height;
    attr.stVencAttr.u32VirWidth = width;
    attr.stVencAttr.u32VirHeight = height;
    attr.stVencAttr.u32StreamBufCnt = 4;
    attr.stVencAttr.u32BufSize = width * height / 2; /* largest IDR it can hold */

    VENC_H264_CBR_S cbr; /* H.265 CBR is the same struct */
    memset(&cbr, 0, sizeof cbr);
    cbr.u32Gop = c->gop;
    cbr.u32BitRate = kbps;
    cbr.u32SrcFrameRateNum = c->fps;
    cbr.u32SrcFrameRateDen = 1;
    cbr.fr32DstFrameRateNum = c->fps;
    cbr.fr32DstFrameRateDen = 1;
    if (h265) {
        attr.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
        attr.stRcAttr.stH265Cbr = cbr;
    } else {
        attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
        attr.stRcAttr.stH264Cbr = cbr;
    }
    attr.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;

    RK_S32 ret = RK_MPI_VENC_CreateChn(chn, &attr);
    if (ret != RK_SUCCESS) {
        LOGE("VENC %d CreateChn: %#x", chn, ret);
        return -1;
    }
    if (chn == VENC_MAIN)
        cam.venc_main = true;
    else
        cam.venc_sub = true;

    if (wrap) {
        VENC_CHN_BUF_WRAP_S w;
        memset(&w, 0, sizeof w);
        w.bEnable = RK_TRUE;
        w.u32BufLine = wrap_lines();
        if ((ret = RK_MPI_VENC_SetChnBufWrapAttr(chn, &w)) != RK_SUCCESS) {
            LOGE("VENC SetChnBufWrapAttr: %#x", ret);
            return -1;
        }
    }
    /* Reconstructed and reference frames share one buffer: about a frame less
     * of CMA. */
    VENC_CHN_REF_BUF_SHARE_S share;
    memset(&share, 0, sizeof share);
    share.bEnable = RK_TRUE;
    RK_MPI_VENC_SetChnRefBufShareAttr(chn, &share);

    VENC_RECV_PIC_PARAM_S recv;
    memset(&recv, 0, sizeof recv);
    recv.s32RecvPicNum = -1; /* forever */
    if ((ret = RK_MPI_VENC_StartRecvFrame(chn, &recv)) != RK_SUCCESS) {
        LOGE("VENC %d StartRecvFrame: %#x", chn, ret);
        return -1;
    }
    return 0;
}

/* The JPEG encoder runs in combo with VENC 0: it encodes the same input
 * frames, only when asked, without a separate VI channel. */
static int venc_jpeg_start(void) {
    const cam_config *c = &cam.cfg;
    VENC_CHN_ATTR_S attr;
    memset(&attr, 0, sizeof attr);
    attr.stVencAttr.enType = RK_VIDEO_ID_JPEG;
    attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    attr.stVencAttr.u32MaxPicWidth = c->width;
    attr.stVencAttr.u32MaxPicHeight = c->height;
    attr.stVencAttr.u32PicWidth = c->width;
    attr.stVencAttr.u32PicHeight = c->height;
    attr.stVencAttr.u32VirWidth = c->width;
    attr.stVencAttr.u32VirHeight = c->height;
    attr.stVencAttr.u32StreamBufCnt = 1;
    attr.stVencAttr.u32BufSize = c->width * c->height / 4;
    attr.stVencAttr.stAttrJpege.enReceiveMode = VENC_PIC_RECEIVE_SINGLE;
    attr.stGopAttr.enGopMode = VENC_GOPMODE_INIT;

    RK_S32 ret = RK_MPI_VENC_CreateChn(VENC_JPEG, &attr);
    if (ret != RK_SUCCESS) {
        LOGE("JPEG CreateChn: %#x", ret);
        return -1;
    }
    cam.venc_jpeg = true;

    VENC_JPEG_PARAM_S jp;
    memset(&jp, 0, sizeof jp);
    jp.u32Qfactor = c->jpeg_quality;
    RK_MPI_VENC_SetJpegParam(VENC_JPEG, &jp);
    if (c->wrap) {
        VENC_CHN_BUF_WRAP_S wrap;
        memset(&wrap, 0, sizeof wrap);
        wrap.bEnable = RK_TRUE;
        wrap.u32BufLine = wrap_lines();
        RK_MPI_VENC_SetChnBufWrapAttr(VENC_JPEG, &wrap);
    }
    VENC_COMBO_ATTR_S combo;
    memset(&combo, 0, sizeof combo);
    combo.bEnable = RK_TRUE;
    combo.s32ChnId = VENC_MAIN;
    if ((ret = RK_MPI_VENC_SetComboAttr(VENC_JPEG, &combo)) != RK_SUCCESS) {
        LOGE("JPEG SetComboAttr: %#x", ret);
        return -1;
    }
    /* The combo channel must have been started once before VENC 0 runs
     * (rkipc: "must, for no streams callback running failed"). It encodes one
     * picture now, which the first snapshot discards. */
    VENC_RECV_PIC_PARAM_S recv;
    memset(&recv, 0, sizeof recv);
    recv.s32RecvPicNum = 1;
    RK_MPI_VENC_StartRecvFrame(VENC_JPEG, &recv);
    return 0;
}

/* ---- encoder thread ------------------------------------------------------- */

static bool is_keyframe(const VENC_PACK_S *p, bool h265) {
    if (h265)
        return p->DataType.enH265EType == H265E_NALU_IDRSLICE ||
               p->DataType.enH265EType == H265E_NALU_ISLICE;
    return p->DataType.enH264EType == H264E_NALU_IDRSLICE ||
           p->DataType.enH264EType == H264E_NALU_ISLICE;
}

static void *encoder_loop(void *arg) {
    const int stream_idx = (int)(intptr_t)arg;
    const bool sub = stream_idx == CAM_STREAM_SUB;
    const int chn = sub ? VENC_SUB : VENC_MAIN;
    const bool h265 = sub ? cam.cfg.sub_h265 : cam.cfg.h265;
    prctl(PR_SET_NAME, sub ? "venc-sub" : "venc-main", 0, 0, 0);
    VENC_PACK_S pack;
    VENC_STREAM_S stream;
    unsigned consecutive_errors = 0;

    while (cam.running) {
        memset(&stream, 0, sizeof stream);
        stream.pstPack = &pack;
        stream.u32PackCount = 1;
        /* Bounded wait so cam_close() is noticed. */
        RK_S32 ret = RK_MPI_VENC_GetStream(chn, &stream, 500);
        if (ret != RK_SUCCESS) {
            pthread_mutex_lock(&cam.stats_mu);
            cam.stats.get_errors++;
            pthread_mutex_unlock(&cam.stats_mu);
            /* One timeout is normal at start; a run of them means the sensor
             * stopped. */
            if (++consecutive_errors == 10)
                LOGW("%s: no encoded frames for 5 s (%#x)", sub ? "sub" : "main", ret);
            continue;
        }
        if (consecutive_errors >= 10)
            LOGI("encoder output resumed");
        consecutive_errors = 0;

        const uint8_t *data = RK_MPI_MB_Handle2VirAddr(pack.pMbBlk);
        bool key = is_keyframe(&pack, h265);
        if (data && pack.u32Len > 0)
            cam.on_frame(cam.user, stream_idx, data, pack.u32Len, pack.u64PTS, key);
        RK_MPI_VENC_ReleaseStream(chn, &stream);

        pthread_mutex_lock(&cam.stats_mu);
        if (sub) {
            cam.stats.sub_frames++;
            cam.stats.sub_bytes += pack.u32Len;
        } else {
            cam.stats.frames++;
            cam.stats.bytes += pack.u32Len;
            cam.stats.keyframes += key;
        }
        pthread_mutex_unlock(&cam.stats_mu);
    }
    return NULL;
}

/* ---- public --------------------------------------------------------------- */

int cam_open(const cam_config *cfg, cam_frame_fn on_frame, void *user) {
    cam.cfg = *cfg;
    cam.on_frame = on_frame;
    cam.user = user;
    memset(&cam.stats, 0, sizeof cam.stats);

    /* rkaiq first: it configures the sensor and ISP that VI then opens. */
    if (isp_start() != 0)
        return -1;
    RK_S32 ret = RK_MPI_SYS_Init();
    if (ret != RK_SUCCESS) {
        LOGE("RK_MPI_SYS_Init: %#x", ret);
        goto fail;
    }
    cam.sys_init = true;
    if (vi_start() != 0 ||
        venc_video_start(VENC_MAIN, cfg->h265, cfg->width, cfg->height, cfg->bitrate_kbps,
                         cfg->wrap) != 0)
        goto fail;
    if (cfg->jpeg_quality > 0 && venc_jpeg_start() != 0) {
        LOGW("snapshots disabled: the JPEG encoder could not be set up");
        if (cam.venc_jpeg) {
            RK_MPI_VENC_DestroyChn(VENC_JPEG);
            cam.venc_jpeg = false;
        }
    }

    MPP_CHN_S src = {.enModId = RK_ID_VI, .s32DevId = VI_DEV, .s32ChnId = VI_CHN};
    MPP_CHN_S dst = {.enModId = RK_ID_VENC, .s32DevId = 0, .s32ChnId = VENC_MAIN};
    if ((ret = RK_MPI_SYS_Bind(&src, &dst)) != RK_SUCCESS) {
        LOGE("bind VI -> VENC: %#x", ret);
        goto fail;
    }
    cam.bound = true;

    if (cfg->sub) {
        if (vi_sub_start() != 0 ||
            venc_video_start(VENC_SUB, cfg->sub_h265, cfg->sub_width, cfg->sub_height,
                             cfg->sub_bitrate_kbps, false) != 0)
            goto fail;
        MPP_CHN_S ssrc = {.enModId = RK_ID_VI, .s32DevId = VI_DEV, .s32ChnId = VI_CHN_SUB};
        MPP_CHN_S sdst = {.enModId = RK_ID_VENC, .s32DevId = 0, .s32ChnId = VENC_SUB};
        if ((ret = RK_MPI_SYS_Bind(&ssrc, &sdst)) != RK_SUCCESS) {
            LOGE("bind VI -> VENC (sub): %#x", ret);
            goto fail;
        }
        cam.bound_sub = true;
    }

    cam.running = true;
    if ((errno = pthread_create(&cam.thread, NULL, encoder_loop,
                                (void *)(intptr_t)CAM_STREAM_MAIN)) != 0) {
        LOGE("encoder thread: %s", strerror(errno));
        cam.running = false;
        goto fail;
    }
    if (cfg->sub) {
        if ((errno = pthread_create(&cam.sub_thread, NULL, encoder_loop,
                                    (void *)(intptr_t)CAM_STREAM_SUB)) != 0) {
            LOGE("sub encoder thread: %s", strerror(errno));
            goto fail;
        }
        cam.sub_thread_started = true;
    }
    LOGI("%dx%d@%d %s CBR %d kbit/s, GOP %d%s%s", cfg->width, cfg->height, cfg->fps,
         cfg->h265 ? "H.265" : "H.264", cfg->bitrate_kbps, cfg->gop,
         cfg->wrap ? ", wrap" : "", cam.venc_jpeg ? ", JPEG snapshots" : "");
    if (cfg->sub)
        LOGI("sub stream %dx%d@%d %s CBR %d kbit/s", cfg->sub_width, cfg->sub_height, cfg->fps,
             cfg->sub_h265 ? "H.265" : "H.264", cfg->sub_bitrate_kbps);
    return 0;

fail:
    cam_close();
    return -1;
}

void cam_close(void) {
    if (cam.running) {
        cam.running = false;
        pthread_join(cam.thread, NULL);
        if (cam.sub_thread_started)
            pthread_join(cam.sub_thread, NULL);
        cam.sub_thread_started = false;
    }
    if (cam.bound_sub) {
        MPP_CHN_S src = {.enModId = RK_ID_VI, .s32DevId = VI_DEV, .s32ChnId = VI_CHN_SUB};
        MPP_CHN_S dst = {.enModId = RK_ID_VENC, .s32DevId = 0, .s32ChnId = VENC_SUB};
        RK_MPI_SYS_UnBind(&src, &dst);
        cam.bound_sub = false;
    }
    if (cam.vi_sub) {
        RK_MPI_VI_DisableChn(VI_PIPE, VI_CHN_SUB);
        cam.vi_sub = false;
    }
    if (cam.venc_sub) {
        RK_MPI_VENC_StopRecvFrame(VENC_SUB);
        RK_MPI_VENC_DestroyChn(VENC_SUB);
        cam.venc_sub = false;
    }
    if (cam.bound) {
        MPP_CHN_S src = {.enModId = RK_ID_VI, .s32DevId = VI_DEV, .s32ChnId = VI_CHN};
        MPP_CHN_S dst = {.enModId = RK_ID_VENC, .s32DevId = 0, .s32ChnId = VENC_MAIN};
        RK_MPI_SYS_UnBind(&src, &dst);
        cam.bound = false;
    }
    if (cam.vi_chn) {
        RK_MPI_VI_DisableChn(VI_PIPE, VI_CHN);
        cam.vi_chn = false;
    }
    if (cam.venc_jpeg) {
        RK_MPI_VENC_StopRecvFrame(VENC_JPEG);
        RK_MPI_VENC_DestroyChn(VENC_JPEG);
        cam.venc_jpeg = false;
    }
    if (cam.venc_main) {
        RK_MPI_VENC_StopRecvFrame(VENC_MAIN);
        RK_MPI_VENC_DestroyChn(VENC_MAIN);
        cam.venc_main = false;
    }
    if (cam.vi_dev) {
        RK_MPI_VI_DisableDev(VI_DEV);
        cam.vi_dev = false;
    }
    if (cam.sys_init) {
        RK_MPI_SYS_Exit();
        cam.sys_init = false;
    }
    isp_stop();
}

void cam_request_idr(int stream) {
    if (stream == CAM_STREAM_SUB) {
        if (cam.venc_sub)
            RK_MPI_VENC_RequestIDR(VENC_SUB, RK_TRUE);
    } else if (cam.venc_main) {
        RK_MPI_VENC_RequestIDR(VENC_MAIN, RK_TRUE);
    }
}

int cam_snapshot(void (*fn)(void *user, const uint8_t *jpeg, size_t len), void *user,
                 int timeout_ms) {
    if (!cam.venc_jpeg)
        return -1;
    VENC_PACK_S pack;
    VENC_STREAM_S stream;

    /* Drop a picture left over from an earlier request (or the start-up one):
     * it shows the past. */
    for (;;) {
        memset(&stream, 0, sizeof stream);
        stream.pstPack = &pack;
        stream.u32PackCount = 1;
        if (RK_MPI_VENC_GetStream(VENC_JPEG, &stream, 0) != RK_SUCCESS)
            break;
        RK_MPI_VENC_ReleaseStream(VENC_JPEG, &stream);
    }

    VENC_RECV_PIC_PARAM_S recv;
    memset(&recv, 0, sizeof recv);
    recv.s32RecvPicNum = 1;
    RK_MPI_VENC_StopRecvFrame(VENC_JPEG);
    RK_S32 ret = RK_MPI_VENC_StartRecvFrame(VENC_JPEG, &recv);
    if (ret != RK_SUCCESS) {
        LOGW("JPEG StartRecvFrame: %#x", ret);
        return -1;
    }
    memset(&stream, 0, sizeof stream);
    stream.pstPack = &pack;
    stream.u32PackCount = 1;
    ret = RK_MPI_VENC_GetStream(VENC_JPEG, &stream, timeout_ms);
    if (ret != RK_SUCCESS) {
        LOGW("no JPEG within %d ms (%#x)", timeout_ms, ret);
        return -1;
    }
    const uint8_t *data = RK_MPI_MB_Handle2VirAddr(pack.pMbBlk);
    int rc = -1;
    if (data && pack.u32Len > 0) {
        fn(user, data, pack.u32Len);
        rc = 0;
    }
    RK_MPI_VENC_ReleaseStream(VENC_JPEG, &stream);
    return rc;
}

const char *cam_sensor_name(void) { return cam.sensor; }

int cam_probe_sensor_model(char *buf, size_t len) {
    rk_aiq_static_info_t info;
    memset(&info, 0, sizeof info);
    if (len == 0 ||
        rk_aiq_uapi2_sysctl_enumStaticMetasByPhyId(CAM_ID, &info) != XCAM_RETURN_NO_ERROR ||
        !info.sensor_info.sensor_name[0])
        return -1;
    /* The entity is "m00_b_sc3336 4-0030": module index, facing, then the
     * driver name, which is the sensor's part number. */
    const char *name = info.sensor_info.sensor_name;
    const char *start = name;
    for (int fields = 0; fields < 2; fields++) {
        const char *u = strchr(start, '_');
        if (!u)
            break;
        start = u + 1;
    }
    size_t n = 0;
    for (; start[n] && start[n] != ' ' && n + 1 < len; n++)
        buf[n] = (char)toupper((unsigned char)start[n]);
    buf[n] = '\0';
    return n > 0 ? 0 : -1;
}

void cam_get_stats(cam_stats *s) {
    pthread_mutex_lock(&cam.stats_mu);
    *s = cam.stats;
    pthread_mutex_unlock(&cam.stats_mu);
}
