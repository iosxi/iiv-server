/* mfdec.c - Windows 標準の H.264 デコーダ(Microsoft H264 Video Decoder MFT)で復号し、元の NV12 と比べる(実験)
 *
 *   mfdec <in.bin> <in.bin.sizes> <ref.nv12> <w> <h> [dxva=0]
 *
 *   1 フレームずつ入れて出てくるまでの時間を測る。dxva=0 なら CPU で復号してメモリに出し、元の絵との
 *   PSNR(Y・UV)を出す。dxva=1 なら GPU(DXVA)で復号する(絵は GPU にあるので PSNR は出さない)。
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
#include <math.h>
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
#pragma comment(lib, "dxguid.lib")

#define CK(x) do { HRESULT _h = (x); if (FAILED(_h)) { printf("NG %s: 0x%08lX (行 %d)\n", #x, _h, __LINE__); exit(1); } } while (0)

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

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static IMFTransform *g_mft;
static UINT32 g_ow, g_oh;
static BOOL g_provides;
static DWORD g_outSize;

static void set_out_type(void)
{
    IMFMediaType *t = NULL;
    DWORD i;
    MFT_OUTPUT_STREAM_INFO si;
    for (i = 0; SUCCEEDED(IMFTransform_GetOutputAvailableType(g_mft, 0, i, &t)); i++) {
        GUID sub;
        IMFMediaType_GetGUID(t, &MF_MT_SUBTYPE, &sub);
        if (IsEqualGUID(&sub, &MFVideoFormat_NV12)) {
            UINT64 fs = 0;
            CK(IMFTransform_SetOutputType(g_mft, 0, t, 0));
            IMFMediaType_GetUINT64(t, &MF_MT_FRAME_SIZE, &fs);
            g_ow = (UINT32)(fs >> 32); g_oh = (UINT32)fs;
            IMFMediaType_Release(t);
            break;
        }
        IMFMediaType_Release(t);
    }
    IMFTransform_GetOutputStreamInfo(g_mft, 0, &si);
    g_provides = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    g_outSize = si.cbSize;
}

int main(int argc, char **argv)
{
    FILE *f;
    BYTE *bits, *ref;
    size_t bitsLen, refLen, fsize, off = 0;
    int w, h, dxva, nf = 0, nout = 0, i;
    unsigned sizes[4096];
    double lat[4096], t0, c0, tEnd, c1, sy = 0, suv = 0;
    int npsnr = 0;
    IMFActivate **act = NULL;
    UINT32 nact = 0;
    MFT_REGISTER_TYPE_INFO inInfo = { MFMediaType_Video, MFVideoFormat_H264 };
    IMFMediaType *mt;
    ICodecAPI *api = NULL;

    if (argc < 6) { printf("使い方: mfdec in.bin in.bin.sizes ref.nv12 w h [dxva]\n"); return 2; }
    SetConsoleOutputCP(CP_UTF8);
    w = atoi(argv[4]); h = atoi(argv[5]); dxva = argc > 6 ? atoi(argv[6]) : 0;
    fsize = (size_t)w * h * 3 / 2;
    f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); bitsLen = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    bits = (BYTE *)malloc(bitsLen); fread(bits, 1, bitsLen, f); fclose(f);
    f = fopen(argv[2], "r"); while (nf < 4096 && fscanf(f, "%u", &sizes[nf]) == 1) nf++; fclose(f);
    f = fopen(argv[3], "rb"); fseek(f, 0, SEEK_END); refLen = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    ref = (BYTE *)malloc(refLen); fread(ref, 1, refLen, f); fclose(f);

    CK(CoInitializeEx(NULL, COINIT_MULTITHREADED));
    CK(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    CK(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &inInfo, NULL, &act, &nact));
    if (!nact) { printf("デコーダが無い\n"); return 1; }
    CK(IMFActivate_ActivateObject(act[0], &IID_IMFTransform, (void **)&g_mft));
    if (dxva) {
        ID3D11Device *dev = NULL;
        ID3D11DeviceContext *ctx = NULL;
        IMFDXGIDeviceManager *mgr = NULL;
        ID3D10Multithread *mtp = NULL;
        UINT token;
        D3D_FEATURE_LEVEL fl;
        CK(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, NULL, 0,
                             D3D11_SDK_VERSION, &dev, &fl, &ctx));
        if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_ID3D10Multithread, (void **)&mtp))) {
            ID3D10Multithread_SetMultithreadProtected(mtp, TRUE);
            ID3D10Multithread_Release(mtp);
        }
        CK(MFCreateDXGIDeviceManager(&token, &mgr));
        CK(IMFDXGIDeviceManager_ResetDevice(mgr, (IUnknown *)dev, token));
        CK(IMFTransform_ProcessMessage(g_mft, MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)mgr));
    }
    if (SUCCEEDED(IMFTransform_QueryInterface(g_mft, &IID_ICodecAPI, (void **)&api))) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI4; v.ulVal = 1;
        if (FAILED(ICodecAPI_SetValue(api, &CODECAPI_AVLowLatencyMode, &v))) printf("  (低遅延にできない)\n");
    }
    CK(MFCreateMediaType(&mt));
    IMFMediaType_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(mt, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
    IMFMediaType_SetUINT64(mt, &MF_MT_FRAME_SIZE, ((UINT64)w << 32) | (UINT32)h);
    IMFMediaType_SetUINT64(mt, &MF_MT_FRAME_RATE, ((UINT64)60 << 32) | 1);
    IMFMediaType_SetUINT32(mt, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    CK(IMFTransform_SetInputType(g_mft, 0, mt, 0));
    set_out_type();
    IMFTransform_ProcessMessage(g_mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);

    t0 = now_ms();
    c0 = cpu_ms();
    for (i = 0; i < nf; i++) {
        IMFSample *s;
        IMFMediaBuffer *b;
        BYTE *p;
        double ti;
        CK(MFCreateMemoryBuffer(sizes[i], &b));
        IMFMediaBuffer_Lock(b, &p, NULL, NULL); memcpy(p, bits + off, sizes[i]); IMFMediaBuffer_Unlock(b);
        IMFMediaBuffer_SetCurrentLength(b, sizes[i]);
        off += sizes[i];
        MFCreateSample(&s); IMFSample_AddBuffer(s, b);
        IMFSample_SetSampleTime(s, (LONGLONG)i * 166667); IMFSample_SetSampleDuration(s, 166667);
        ti = now_ms();
        CK(IMFTransform_ProcessInput(g_mft, 0, s, 0));
        IMFSample_Release(s); IMFMediaBuffer_Release(b);
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER odb;
            DWORD st = 0;
            HRESULT hr;
            ZeroMemory(&odb, sizeof(odb));
            if (!g_provides) {
                IMFMediaBuffer *ob;
                MFCreateSample(&odb.pSample);
                MFCreateMemoryBuffer(g_outSize ? g_outSize : (DWORD)(g_ow * g_oh * 3 / 2), &ob);
                IMFSample_AddBuffer(odb.pSample, ob);
                IMFMediaBuffer_Release(ob);
            }
            hr = IMFTransform_ProcessOutput(g_mft, 0, 1, &odb, &st);
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (odb.pSample) IMFSample_Release(odb.pSample);
                set_out_type();
                continue;
            }
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) { if (odb.pSample) IMFSample_Release(odb.pSample); break; }
            CK(hr);
            if (nout < 4096) lat[nout] = now_ms() - ti;
            if (!dxva && nout < (int)(refLen / fsize) * 100) {
                IMFMediaBuffer *ob = NULL;
                BYTE *q;
                DWORD len;
                const BYTE *r = ref + (size_t)(nout % (int)(refLen / fsize)) * fsize;
                double ey = 0, euv = 0;
                int x, y;
                LONG pitch = (LONG)g_ow;
                IMF2DBuffer *b2 = NULL;
                IMFSample_ConvertToContiguousBuffer(odb.pSample, &ob);
                if (SUCCEEDED(IMFMediaBuffer_QueryInterface(ob, &IID_IMF2DBuffer, (void **)&b2))) {
                    IMF2DBuffer_Lock2D(b2, &q, &pitch);
                } else {
                    IMFMediaBuffer_Lock(ob, &q, NULL, &len);
                }
                for (y = 0; y < h; y++)
                    for (x = 0; x < w; x++) { double d = (double)q[(size_t)y * pitch + x] - r[(size_t)y * w + x]; ey += d * d; }
                for (y = 0; y < h / 2; y++)
                    for (x = 0; x < w; x++) {
                        double d = (double)q[(size_t)(g_oh + y) * pitch + x] - r[(size_t)(h + y) * w + x];
                        euv += d * d;
                    }
                ey /= (double)w * h; euv /= (double)w * h / 2;
                sy += ey ? 10 * log10(255.0 * 255.0 / ey) : 99;
                suv += euv ? 10 * log10(255.0 * 255.0 / euv) : 99;
                npsnr++;
                if (b2) { IMF2DBuffer_Unlock2D(b2); IMF2DBuffer_Release(b2); } else IMFMediaBuffer_Unlock(ob);
                IMFMediaBuffer_Release(ob);
            }
            nout++;
            if (odb.pSample) IMFSample_Release(odb.pSample);
            if (odb.pEvents) IMFCollection_Release(odb.pEvents);
        }
    }
    tEnd = now_ms();
    c1 = cpu_ms();
    qsort(lat, (size_t)nout, sizeof(double), cmp_d);
    printf("デコード(%s): 入れた %d 出た %d  1 フレーム 中央 %.2fms 95%% %.2fms  CPU %.1f ms/フレーム  出力 %ux%u\n",
           dxva ? "DXVA" : "CPU", nf, nout, nout ? lat[nout / 2] : 0, nout ? lat[nout * 95 / 100] : 0,
           (c1 - c0) / (nout ? nout : 1), g_ow, g_oh);
    if (npsnr) printf("  PSNR Y %.2f dB  UV %.2f dB(元の NV12 と、%d フレームの平均)\n", sy / npsnr, suv / npsnr, npsnr);
    (void)tEnd;
    return 0;
}
