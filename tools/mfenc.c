/* mfenc.c - Media Foundation のハードウェア H.264 / HEVC エンコーダの遅れ・大きさ・CPU を測る(実験)
 *
 *   mfenc <in.nv12> <w> <h> <frames> <out.bin> <cbr|qp|qp2|qps|q> <値(kbps / QP / 画質)> [paced=1] [tex=1] [hevc=0]
 *   qp  = 画質一定 + QP を型より前に、qp2 = 型の後に、qps = フレームごとの属性で、q = 画質(0〜100)
 *   qdyn = 画質 40 で始め、半分のフレームから値(0〜100)へ上げる(途中で変えて効くか)
 *
 *   in.nv12 は NV12 の連番。paced=1 なら 60fps の間隔で入れ(遅れを測る)、0 なら求められるだけ速く入れる。
 *   tex=1 なら D3D11 の NV12 テクスチャで渡す(取り込みと同じ形)、0 ならメモリの NV12。
 *   out.bin は Annex-B のビット列、out.bin.sizes は 1 行 1 フレームの大きさ。
 */
#define COBJMACROS
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <strmif.h>
#include <stdio.h>
#include <stdlib.h>
#include <initguid.h>
#include <codecapi.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "dxguid.lib")

#define CK(x) do { HRESULT _h = (x); if (FAILED(_h)) { printf("NG %s: 0x%08lX (行 %d)\n", #x, _h, __LINE__); exit(1); } } while (0)
#define NTEX 8

static double now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static double cpu_ms(void)
{
    FILETIME a, b, k, u;
    GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u);
    return ((double)(((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) +
            (double)(((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 10000.0;
}

static void set_u32(ICodecAPI *api, const GUID *g, ULONG v, const char *name)
{
    VARIANT var;
    HRESULT hr;
    VariantInit(&var);
    var.vt = VT_UI4;
    var.ulVal = v;
    hr = ICodecAPI_SetValue(api, g, &var);
    if (FAILED(hr)) printf("  (設定できない %s = %lu: 0x%08lX)\n", name, v, hr);
}

static void set_bool(ICodecAPI *api, const GUID *g, BOOL v, const char *name)
{
    VARIANT var;
    HRESULT hr;
    VariantInit(&var);
    var.vt = VT_BOOL;
    var.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
    hr = ICodecAPI_SetValue(api, g, &var);
    if (FAILED(hr)) printf("  (設定できない %s: 0x%08lX)\n", name, hr);
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    const char *inPath, *outPath, *mode;
    int w, h, frames, val, paced, useTex, hevc;
    size_t fsize, inLen;
    BYTE *in;
    FILE *f, *fo, *fs;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    ID3D11Texture2D *tex[NTEX] = { 0 };
    IMFDXGIDeviceManager *mgr = NULL;
    UINT token = 0;
    IMFActivate **act = NULL;
    UINT32 nact = 0, i;
    IMFTransform *mft = NULL;
    IMFAttributes *attr = NULL;
    IMFMediaEventGenerator *gen = NULL;
    ICodecAPI *api = NULL;
    IMFMediaType *mt;
    MFT_REGISTER_TYPE_INFO outInfo;
    WCHAR *name = NULL;
    UINT32 nameLen;
    double *tIn, *lat, t0, c0, tEnd, c1;
    int nin = 0, nout = 0;
    size_t totalBytes = 0, firstBytes = 0;
    D3D_FEATURE_LEVEL fl;

    if (argc < 8) { printf("使い方: mfenc in.nv12 w h frames out.bin cbr|qp 値 [paced] [tex] [hevc]\n"); return 2; }
    SetConsoleOutputCP(CP_UTF8);
    inPath = argv[1]; w = atoi(argv[2]); h = atoi(argv[3]); frames = atoi(argv[4]); outPath = argv[5];
    mode = argv[6]; val = atoi(argv[7]);
    paced = argc > 8 ? atoi(argv[8]) : 1;
    useTex = argc > 9 ? atoi(argv[9]) : 1;
    hevc = argc > 10 ? atoi(argv[10]) : 0;
    fsize = (size_t)w * h * 3 / 2;

    f = fopen(inPath, "rb");
    if (!f) { printf("読めない %s\n", inPath); return 1; }
    fseek(f, 0, SEEK_END); inLen = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    in = (BYTE *)malloc(inLen);
    fread(in, 1, inLen, f);
    fclose(f);
    if (inLen / fsize < 1) { printf("フレームが無い\n"); return 1; }

    timeBeginPeriod(1);
    CK(CoInitializeEx(NULL, COINIT_MULTITHREADED));
    CK(MFStartup(MF_VERSION, MFSTARTUP_LITE));

    CK(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, NULL, 0,
                         D3D11_SDK_VERSION, &dev, &fl, &ctx));
    {
        ID3D10Multithread *mtp = NULL;
        if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_ID3D10Multithread, (void **)&mtp))) {
            ID3D10Multithread_SetMultithreadProtected(mtp, TRUE);
            ID3D10Multithread_Release(mtp);
        }
    }
    CK(MFCreateDXGIDeviceManager(&token, &mgr));
    CK(IMFDXGIDeviceManager_ResetDevice(mgr, (IUnknown *)dev, token));
    if (useTex) {
        D3D11_TEXTURE2D_DESC td;
        ZeroMemory(&td, sizeof(td));
        td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (i = 0; i < NTEX; i++) CK(ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex[i]));
    }

    outInfo.guidMajorType = MFMediaType_Video;
    outInfo.guidSubtype = hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264;
    CK(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, NULL, &outInfo, &act, &nact));
    if (!nact) { printf("ハードウェアのエンコーダが無い\n"); return 1; }
    IMFActivate_GetAllocatedString(act[0], &MFT_FRIENDLY_NAME_Attribute, &name, &nameLen);
    printf("エンコーダ: %ls  %dx%d  %s %d  %s  %s\n", name, w, h, mode, val, paced ? "60fps の間隔" : "全速", useTex ? "テクスチャ" : "メモリ");
    CK(IMFActivate_ActivateObject(act[0], &IID_IMFTransform, (void **)&mft));
    CK(IMFTransform_GetAttributes(mft, &attr));
    CK(IMFAttributes_SetUINT32(attr, &MF_TRANSFORM_ASYNC_UNLOCK, TRUE));
    CK(IMFTransform_ProcessMessage(mft, MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)mgr));
    CK(IMFTransform_QueryInterface(mft, &IID_ICodecAPI, (void **)&api));

    set_bool(api, &CODECAPI_AVLowLatencyMode, TRUE, "LowLatency");
    set_u32(api, &CODECAPI_AVEncMPVDefaultBPictureCount, 0, "BFrames");
    set_u32(api, &CODECAPI_AVEncMPVGOPSize, 0xFFFFFFFF, "GOP");
    if (!strcmp(mode, "qp") || !strcmp(mode, "qp2") || !strcmp(mode, "qps") || !strcmp(mode, "q") || !strcmp(mode, "qdyn")) {
        set_u32(api, &CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_Quality, "RC=Quality");
        if (!strcmp(mode, "qp")) set_u32(api, &CODECAPI_AVEncVideoEncodeQP, (ULONG)val, "QP");
        if (!strcmp(mode, "q")) set_u32(api, &CODECAPI_AVEncCommonQuality, (ULONG)val, "Quality");
        if (!strcmp(mode, "qdyn")) set_u32(api, &CODECAPI_AVEncCommonQuality, 40, "Quality");
    } else {
        set_u32(api, &CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR, "RC=CBR");
        set_u32(api, &CODECAPI_AVEncCommonMeanBitRate, (ULONG)val * 1000, "Bitrate");
    }

    CK(MFCreateMediaType(&mt));
    IMFMediaType_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(mt, &MF_MT_SUBTYPE, hevc ? &MFVideoFormat_HEVC : &MFVideoFormat_H264);
    IMFMediaType_SetUINT32(mt, &MF_MT_AVG_BITRATE, !strcmp(mode, "qp") ? 50000000 : (UINT32)val * 1000);
    IMFMediaType_SetUINT64(mt, &MF_MT_FRAME_SIZE, ((UINT64)w << 32) | (UINT32)h);
    IMFMediaType_SetUINT64(mt, &MF_MT_FRAME_RATE, ((UINT64)60 << 32) | 1);
    IMFMediaType_SetUINT64(mt, &MF_MT_PIXEL_ASPECT_RATIO, ((UINT64)1 << 32) | 1);
    IMFMediaType_SetUINT32(mt, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (!hevc) IMFMediaType_SetUINT32(mt, &MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    CK(IMFTransform_SetOutputType(mft, 0, mt, 0));
    IMFMediaType_Release(mt);

    CK(MFCreateMediaType(&mt));
    IMFMediaType_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(mt, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);
    IMFMediaType_SetUINT64(mt, &MF_MT_FRAME_SIZE, ((UINT64)w << 32) | (UINT32)h);
    IMFMediaType_SetUINT64(mt, &MF_MT_FRAME_RATE, ((UINT64)60 << 32) | 1);
    IMFMediaType_SetUINT32(mt, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    CK(IMFTransform_SetInputType(mft, 0, mt, 0));
    IMFMediaType_Release(mt);

    if (!strcmp(mode, "qp2")) set_u32(api, &CODECAPI_AVEncVideoEncodeQP, (ULONG)val, "QP(型の後)");
    {
        VARIANT v;
        VariantInit(&v);
        if (SUCCEEDED(ICodecAPI_GetValue(api, &CODECAPI_AVEncVideoEncodeQP, &v))) printf("  読み返した QP: %lu (vt %d)\n", v.ulVal, v.vt);
        else printf("  QP は読み返せない\n");
    }
    CK(IMFTransform_QueryInterface(mft, &IID_IMFMediaEventGenerator, (void **)&gen));
    CK(IMFTransform_ProcessMessage(mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    CK(IMFTransform_ProcessMessage(mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));

    fo = fopen(outPath, "wb");
    {
        char sp[MAX_PATH];
        sprintf(sp, "%s.sizes", outPath);
        fs = fopen(sp, "w");
    }
    tIn = (double *)calloc((size_t)frames, sizeof(double));
    lat = (double *)calloc((size_t)frames, sizeof(double));
    t0 = now_ms();
    c0 = cpu_ms();
    while (nout < frames) {
        IMFMediaEvent *ev = NULL;
        MediaEventType type;
        if (FAILED(IMFMediaEventGenerator_GetEvent(gen, 0, &ev))) break;
        IMFMediaEvent_GetType(ev, &type);
        IMFMediaEvent_Release(ev);
        if (type == METransformNeedInput && nin < frames) {
            IMFSample *s = NULL;
            IMFMediaBuffer *b = NULL;
            const BYTE *src = in + (size_t)(nin % (int)(inLen / fsize)) * fsize;
            if (paced) {
                double due = t0 + nin * (1000.0 / 60.0);
                while (now_ms() < due - 1.5) Sleep(1);
                while (now_ms() < due) ;
            }
            if (useTex) {
                ID3D11Texture2D *t = tex[nin % NTEX];
                ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)t, 0, NULL, src, (UINT)w, 0);
                CK(MFCreateDXGISurfaceBuffer(&IID_ID3D11Texture2D, (IUnknown *)t, 0, FALSE, &b));
                IMFMediaBuffer_SetCurrentLength(b, (DWORD)fsize);
            } else {
                BYTE *p;
                CK(MFCreateMemoryBuffer((DWORD)fsize, &b));
                IMFMediaBuffer_Lock(b, &p, NULL, NULL);
                memcpy(p, src, fsize);
                IMFMediaBuffer_Unlock(b);
                IMFMediaBuffer_SetCurrentLength(b, (DWORD)fsize);
            }
            CK(MFCreateSample(&s));
            IMFSample_AddBuffer(s, b);
            IMFSample_SetSampleTime(s, (LONGLONG)nin * 166667);
            IMFSample_SetSampleDuration(s, 166667);
            if (!strcmp(mode, "qps")) IMFSample_SetUINT64(s, &MFSampleExtension_VideoEncodeQP, (UINT64)val);
            if (!strcmp(mode, "qdyn") && nin == frames / 2) set_u32(api, &CODECAPI_AVEncCommonQuality, (ULONG)val, "Quality(途中)");
            tIn[nin] = now_ms();
            CK(IMFTransform_ProcessInput(mft, 0, s, 0));
            IMFSample_Release(s);
            IMFMediaBuffer_Release(b);
            nin++;
            if (nin == frames) IMFTransform_ProcessMessage(mft, MFT_MESSAGE_COMMAND_DRAIN, 0);
        } else if (type == METransformHaveOutput) {
            MFT_OUTPUT_DATA_BUFFER odb;
            DWORD status = 0;
            HRESULT hr;
            ZeroMemory(&odb, sizeof(odb));
            hr = IMFTransform_ProcessOutput(mft, 0, 1, &odb, &status);
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                IMFMediaType *ot = NULL;
                IMFTransform_GetOutputAvailableType(mft, 0, 0, &ot);
                IMFTransform_SetOutputType(mft, 0, ot, 0);
                if (ot) IMFMediaType_Release(ot);
                continue;
            }
            if (SUCCEEDED(hr) && odb.pSample) {
                IMFMediaBuffer *b = NULL;
                BYTE *p;
                DWORD len = 0;
                IMFSample_ConvertToContiguousBuffer(odb.pSample, &b);
                IMFMediaBuffer_Lock(b, &p, NULL, &len);
                fwrite(p, 1, len, fo);
                IMFMediaBuffer_Unlock(b);
                IMFMediaBuffer_Release(b);
                fprintf(fs, "%lu\n", len);
                if (nout < nin) lat[nout] = now_ms() - tIn[nout];
                if (nout == 0) firstBytes = len;
                totalBytes += len;
                nout++;
                IMFSample_Release(odb.pSample);
            }
            if (odb.pEvents) IMFCollection_Release(odb.pEvents);
        } else if (type == METransformDrainComplete) {
            break;
        }
    }
    tEnd = now_ms();
    c1 = cpu_ms();
    fclose(fo);
    fclose(fs);
    {
        int n = nout - 1;           /* 最初(IDR)を除く */
        double med, p95, mx;
        qsort(lat + 1, (size_t)n, sizeof(double), cmp_d);
        med = lat[1 + n / 2]; p95 = lat[1 + n * 95 / 100]; mx = lat[nout - 1];
        printf("  出力 %d フレーム  %.1f fps  遅れ 中央 %.2fms 95%% %.2fms 最大 %.2fms(最初 %.2fms)\n",
               nout, nout * 1000.0 / (tEnd - t0), med, p95, mx, lat[0]);
        printf("  最初 %zu バイト、以後 平均 %.1f KB/フレーム = %.1f Mbps(60fps)、CPU %.1f%%(1 コア = 100%%)\n",
               firstBytes, (double)(totalBytes - firstBytes) / (nout - 1) / 1024.0,
               (double)(totalBytes - firstBytes) / (nout - 1) * 8 * 60 / 1e6, (c1 - c0) / (tEnd - t0) * 100);
    }
    return 0;
}
