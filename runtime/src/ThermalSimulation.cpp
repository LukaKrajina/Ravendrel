#include "../include/qhal/v/ThermalSimulation.hpp"

#include <fstream>
#include <string>
#include <limits>

#ifdef _WIN32
#include <windows.h>
#include <wbemidl.h>
#include <comdef.h>
#endif

namespace qhal
{
    HostThermalProbe::Reading HostThermalProbe::synthetic()
    {
        static std::mt19937 rng{7u};
        static double t = 0.0;
        t += 1.0;
        std::normal_distribution<double> jitter(0.0, 1.5);
        Reading r;
        r.cpu_c = 45.0 + 12.0 * std::sin(t / 40.0) + jitter(rng);
        r.gpu_c = 50.0 + 15.0 * std::sin(t / 55.0 + 1.0) + jitter(rng);
        r.synthetic = true;
        return r;
    }

    #ifndef _WIN32
    HostThermalProbe::Reading HostThermalProbe::sample_linux()
    {
        Reading r;
        double cpu_max = -1e9, gpu_max = -1e9;
        bool any = false;
        for (int i = 0; i < 16; ++i)
        {
            std::string path = "/sys/class/thermal/thermal_zone" + std::to_string(i) + "/temp";
            std::ifstream f(path);
            long v = 0;
            if (f >> v)
            {
                cpu_max = std::max(cpu_max, static_cast<double>(v) / 1000.0);
                any = true;
            }
        }

        for (int i = 0; i < 16; ++i)
        {
            std::string base = "/sys/class/hwmon/hwmon" + std::to_string(i);
            for (int j = 1; j <= 3; ++j)
            {
                std::string path = base + "/temp" + std::to_string(j) + "_input";
                std::ifstream f(path);
                long v = 0;
                if (f >> v)
                {
                    gpu_max = std::max(gpu_max, static_cast<double>(v) / 1000.0);
                    any = true;
                }
            }
        }

        if (any)
        {
            r.cpu_c = cpu_max > -1e9 ? cpu_max : 30.0;
            r.gpu_c = gpu_max > -1e9 ? gpu_max : r.cpu_c;
            r.synthetic = false;
        }
        else
        {
            r = synthetic();
        }
        return r;
    }

    #else

    namespace
    {
        double wmi_read_cpu_temperature_celsius()
        {
            using std::numeric_limits;
            double result = numeric_limits<double>::quiet_NaN();

            HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            bool com_initialized = SUCCEEDED(hr);
            if (hr == RPC_E_CHANGED_MODE)
            {
                com_initialized = false;
                hr = S_OK;
            }
            if (FAILED(hr))
                return result;

            hr = CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                                    RPC_C_AUTHN_LEVEL_DEFAULT,
                                    RPC_C_IMP_LEVEL_IMPERSONATE,
                                    nullptr, EOAC_NONE, nullptr);
            if (FAILED(hr) && hr != RPC_E_TOO_LATE)
            {
                if (com_initialized)
                    CoUninitialize();
                return result;
            }

            IWbemLocator *pLoc = nullptr;
            hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWbemLocator, reinterpret_cast<void **>(&pLoc));
            if (SUCCEEDED(hr) && pLoc)
            {
                IWbemServices *pSvc = nullptr;
                hr = pLoc->ConnectServer(_bstr_t(L"ROOT\\WMI"), nullptr, nullptr, nullptr,
                                        0, nullptr, nullptr, &pSvc);
                if (SUCCEEDED(hr) && pSvc)
                {
                    hr = CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                                        RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                                        nullptr, EOAC_NONE);
                    if (SUCCEEDED(hr))
                    {
                        IEnumWbemClassObject *pEnum = nullptr;
                        hr = pSvc->ExecQuery(
                            _bstr_t(L"WQL"),
                            _bstr_t(L"SELECT * FROM MSAcpi_ThermalZoneTemperature"),
                            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                            nullptr, &pEnum);
                        if (SUCCEEDED(hr) && pEnum)
                        {
                            IWbemClassObject *pObj = nullptr;
                            ULONG returned = 0;
                            while (true)
                            {
                                hr = pEnum->Next(WBEM_INFINITE, 1, &pObj, &returned);
                                if (FAILED(hr) || returned == 0)
                                    break;

                                VARIANT vt;
                                VariantInit(&vt);
                                if (SUCCEEDED(pObj->Get(L"CurrentTemperature", 0, &vt, nullptr, nullptr)))
                                {
                                    double kelvin_tenths = 0.0;
                                    if (vt.vt == VT_R8)       kelvin_tenths = vt.dblVal;
                                    else if (vt.vt == VT_I4)  kelvin_tenths = static_cast<double>(vt.lVal);
                                    else if (vt.vt == VT_UI4) kelvin_tenths = static_cast<double>(vt.ulVal);
                                    else if (vt.vt == VT_R4)  kelvin_tenths = static_cast<double>(vt.fltVal);
                                    result = kelvin_tenths / 10.0 - 273.15;
                                    VariantClear(&vt);
                                }
                                pObj->Release();
                                if (!std::isnan(result))
                                    break;
                            }
                            pEnum->Release();
                        }
                    }
                    pSvc->Release();
                }
                pLoc->Release();
            }

            if (com_initialized)
                CoUninitialize();
            return result;
        }

        double nvml_read_gpu_temperature_celsius()
        {
            using std::numeric_limits;
            double result = numeric_limits<double>::quiet_NaN();

            typedef int (*nvmlInit_t)(void);
            typedef int (*nvmlDeviceGetCount_t)(unsigned int *);
            typedef int (*nvmlDeviceGetHandleByIndex_t)(unsigned int, void **);
            typedef int (*nvmlDeviceGetTemperature_t)(void *, int, unsigned int *);
            typedef int (*nvmlShutdown_t)(void);

            HMODULE h = LoadLibraryA("nvml.dll");
            if (!h)
                return result;

            auto init = reinterpret_cast<nvmlInit_t>(GetProcAddress(h, "nvmlInit_v2"));
            auto count = reinterpret_cast<nvmlDeviceGetCount_t>(GetProcAddress(h, "nvmlDeviceGetCount_v2"));
            auto handle = reinterpret_cast<nvmlDeviceGetHandleByIndex_t>(GetProcAddress(h, "nvmlDeviceGetHandleByIndex_v2"));
            auto temp = reinterpret_cast<nvmlDeviceGetTemperature_t>(GetProcAddress(h, "nvmlDeviceGetTemperature"));
            auto shutdown = reinterpret_cast<nvmlShutdown_t>(GetProcAddress(h, "nvmlShutdown"));

            if (init && count && handle && temp && init() == 0)
            {
                unsigned int n = 0;
                if (count(&n) == 0 && n > 0)
                {
                    void *dev = nullptr;
                    unsigned int gpu_temp = 0;
                    if (handle(0, &dev) == 0 && temp(dev, 0 /* NVML_TEMPERATURE_GPU */, &gpu_temp) == 0)
                        result = static_cast<double>(gpu_temp);
                }
                if (shutdown)
                    shutdown();
            }

            FreeLibrary(h);
            return result;
        }
    }

    HostThermalProbe::Reading HostThermalProbe::sample_windows()
    {
        Reading r = synthetic();

        double cpu = wmi_read_cpu_temperature_celsius();
        double gpu = nvml_read_gpu_temperature_celsius();

        if (!std::isnan(cpu))
        {
            r.cpu_c = cpu;
            r.synthetic = false;
        }
        if (!std::isnan(gpu))
        {
            r.gpu_c = gpu;
            r.synthetic = false;
        }
        else if (!std::isnan(cpu))
        {
            r.gpu_c = cpu * 1.15;
        }
        return r;
    }

    #endif

    HostThermalProbe::Reading HostThermalProbe::sample()
    {
    #ifdef _WIN32
        return sample_windows();
    #else
        return sample_linux();
    #endif
    }
}