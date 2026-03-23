#ifndef SPECSENSOR_API_H
#define SPECSENSOR_API_H

#include <cstdint>
#include <memory>
#include <string>

class ISpecSensorApi {
public:
    virtual ~ISpecSensorApi() = default;

    virtual int Load(const std::wstring& license_path) = 0;
    virtual int Unload() = 0;

    virtual int GetDeviceCount(std::int64_t* count) = 0;

    virtual int Open(int device_index) = 0;
    virtual int Close() = 0;

    virtual int Command(const std::wstring& feature) = 0;
    virtual int SetBool(const std::wstring& feature, bool value) = 0;
    virtual int SetFloat(const std::wstring& feature, double value) = 0;
    virtual int SetString(const std::wstring& feature, const std::wstring& value) = 0;
    virtual int SetEnumIndex(const std::wstring& feature, int value) = 0;
    virtual int GetInt(const std::wstring& feature, std::int64_t* value) = 0;
    virtual int GetBool(const std::wstring& feature, bool* value) = 0;
    virtual int GetFloat(const std::wstring& feature, double* value) = 0;
    virtual int GetEnumIndex(const std::wstring& feature, int* value) = 0;
    virtual int GetEnumCount(const std::wstring& feature, int* count) = 0;
    virtual int GetEnumStringByIndex(const std::wstring& feature, int index, std::wstring* value) = 0;

    virtual int CreateBuffer(std::int64_t size_bytes, void** buffer) = 0;
    virtual int DisposeBuffer(void* buffer) = 0;

    virtual int Wait(std::uint8_t* buffer, std::int64_t* frame_size,
                     std::int64_t* frame_number, std::int64_t timeout_ms) = 0;

    virtual const wchar_t* GetErrorString(int code) const = 0;
};

std::unique_ptr<ISpecSensorApi> CreateSpecSensorApi();

#endif
