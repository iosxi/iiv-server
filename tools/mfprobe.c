/* mfprobe.c - Media Foundation の動画エンコーダ・デコーダ(ハードウェア / ソフトウェア)を一覧にする
 *
 *   mfprobe
 */
#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <stdio.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

static void list(const GUID *cat, const char *catName, const GUID *sub, const char *subName, UINT32 flags)
{
    MFT_REGISTER_TYPE_INFO info = { MFMediaType_Video, *sub };
    IMFActivate **act = NULL;
    UINT32 n = 0, i;
    BOOL enc = IsEqualGUID(cat, &MFT_CATEGORY_VIDEO_ENCODER);
    HRESULT hr = MFTEnumEx(*cat, flags, enc ? NULL : &info, enc ? &info : NULL, &act, &n);
    if (FAILED(hr)) { printf("%s %s: MFTEnumEx 0x%08lX\n", catName, subName, hr); return; }
    for (i = 0; i < n; i++) {
        WCHAR *name = NULL;
        UINT32 len = 0, f = 0;
        IMFActivate_GetAllocatedString(act[i], &MFT_FRIENDLY_NAME_Attribute, &name, &len);
        IMFActivate_GetUINT32(act[i], &MF_TRANSFORM_FLAGS_Attribute, &f);
        printf("%s %-5s %s  %ls\n", catName, subName, (f & MFT_ENUM_FLAG_HARDWARE) ? "HW" : "SW", name ? name : L"?");
        CoTaskMemFree(name);
        IMFActivate_Release(act[i]);
    }
    if (!n) printf("%s %-5s (なし)\n", catName, subName);
    CoTaskMemFree(act);
}

int main(void)
{
    UINT32 all = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER;
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    list(&MFT_CATEGORY_VIDEO_ENCODER, "enc", &MFVideoFormat_H264, "H264", all);
    list(&MFT_CATEGORY_VIDEO_ENCODER, "enc", &MFVideoFormat_HEVC, "HEVC", all);
    list(&MFT_CATEGORY_VIDEO_ENCODER, "enc", &MFVideoFormat_AV1, "AV1", all);
    list(&MFT_CATEGORY_VIDEO_DECODER, "dec", &MFVideoFormat_H264, "H264", all);
    list(&MFT_CATEGORY_VIDEO_DECODER, "dec", &MFVideoFormat_HEVC, "HEVC", all);
    list(&MFT_CATEGORY_VIDEO_DECODER, "dec", &MFVideoFormat_AV1, "AV1", all);
    MFShutdown();
    return 0;
}
