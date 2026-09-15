// Copyright (c) 2026 Aanish Bhirud
// SPDX-License-Identifier: MIT
//
// Lists the DirectShow media types a capture client sees for the Cyclops
// virtual camera, without starting capture. This is the same surface
// Discord's background camera survey walks. Build:
//   cl /EHsc /std:c++20 dshow_enum.cpp strmiids.lib ole32.lib oleaut32.lib

#include <windows.h>
#include <dshow.h>
#include <cstdio>
#include <cstring>

static const wchar_t* subtype_name(const GUID& g)
{
    if (g == MEDIASUBTYPE_NV12) return L"NV12";
    if (g == MEDIASUBTYPE_RGB32) return L"RGB32";
    if (g == MEDIASUBTYPE_RGB24) return L"RGB24";
    if (g == MEDIASUBTYPE_YUY2) return L"YUY2";
    if (g == MEDIASUBTYPE_IYUV) return L"IYUV";
    if (g == MEDIASUBTYPE_MJPG) return L"MJPG";
    return L"?";
}

static void fourcc(char out[5], DWORD c)
{
    memcpy(out, &c, 4);
    out[4] = 0;
    for (int i = 0; i < 4; i++)
        if (out[i] < 32 || out[i] > 126)
            out[i] = '.';
}

int main()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
        return 1;

    ICreateDevEnum* devenum = nullptr;
    IEnumMoniker* monikers = nullptr;
    // CreateClassEnumerator returns S_FALSE with a null enumerator when the
    // category is empty; that is not FAILED, so check the pointer too.
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                IID_ICreateDevEnum, (void**)&devenum))
        || devenum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory,
                                          &monikers, 0) != S_OK
        || !monikers)
    {
        wprintf(L"no video input devices\n");
        return 1;
    }

    IMoniker* mon = nullptr;
    bool found = false;
    while (monikers->Next(1, &mon, nullptr) == S_OK)
    {
        IPropertyBag* bag = nullptr;
        wchar_t* name = nullptr;
        if (SUCCEEDED(mon->BindToStorage(nullptr, nullptr, IID_IPropertyBag, (void**)&bag)))
        {
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(bag->Read(L"FriendlyName", &v, nullptr)) && v.vt == VT_BSTR)
            {
                name = wcsdup(v.bstrVal);
                VariantClear(&v);
            }
        }
        const bool ours = name && wcsstr(name, L"Cyclops");
        wprintf(L"device: %hs%ls\n", ours ? ">>> " : "    ", name ? name : L"?");
        if (name)
            free(name);
        if (bag)
            bag->Release();
        if (!ours)
        {
            mon->Release();
            continue;
        }
        found = true;

        IBaseFilter* filter = nullptr;
        if (FAILED(mon->BindToObject(nullptr, nullptr, IID_IBaseFilter, (void**)&filter)))
        {
            mon->Release();
            continue;
        }
        IEnumPins* pins = nullptr;
        filter->EnumPins(&pins);
        IPin* pin = nullptr;
        while (pins && pins->Next(1, &pin, nullptr) == S_OK)
        {
            PIN_DIRECTION dir;
            if (FAILED(pin->QueryDirection(&dir)) || dir != PINDIR_OUTPUT)
            {
                pin->Release();
                continue;
            }
            IEnumMediaTypes* types = nullptr;
            pin->EnumMediaTypes(&types);
            AM_MEDIA_TYPE* mt = nullptr;
            int i = 0;
            while (types && types->Next(1, &mt, nullptr) == S_OK)
            {
                if (mt->formattype == FORMAT_VideoInfo && mt->pbFormat)
                {
                    auto* vih = (VIDEOINFOHEADER*)mt->pbFormat;
                    char cc[5];
                    fourcc(cc, vih->bmiHeader.biCompression);
                    wprintf(L"  type %d: %-6ls w=%ld h=%ld bits=%u comp=%hs sizeImage=%lu\n",
                            i++, subtype_name(mt->subtype),
                            (long)vih->bmiHeader.biWidth, (long)vih->bmiHeader.biHeight,
                            vih->bmiHeader.biBitCount, cc, vih->bmiHeader.biSizeImage);
                }
                else
                {
                    wprintf(L"  type %d: subtype=%ls (non-VideoInfo)\n",
                            i++, subtype_name(mt->subtype));
                }
                if (mt->pbFormat)
                    CoTaskMemFree(mt->pbFormat);
                CoTaskMemFree(mt);
            }
            if (types)
                types->Release();
            pin->Release();
        }
        if (pins)
            pins->Release();
        filter->Release();
        mon->Release();
    }
    monikers->Release();
    devenum->Release();
    if (!found)
        wprintf(L"Cyclops device not found\n");
    CoUninitialize();
    return 0;
}
