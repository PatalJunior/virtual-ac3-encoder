// WasapiCapture.cpp — see WasapiCapture.h.
#include "WasapiCapture.h"

#include <mmreg.h>
#include <ksmedia.h>

#include <avrt.h>
#include <vector>

WasapiCapture::~WasapiCapture()
{
  Stop();
  if (dataEvent_) CloseHandle(dataEvent_);
  if (stopEvent_) CloseHandle(stopEvent_);
}

bool WasapiCapture::Init(IMMDevice* dev, bool loopback, REFERENCE_TIME bufferHns)
{
  loopback_ = loopback;
  bufferHns_ = (bufferHns > 0) ? bufferHns : 100000;
  HR_FAIL(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_), "Activate IAudioClient");

  WAVEFORMATEX* mix = nullptr;
  HR_FAIL(client_->GetMixFormat(&mix), "GetMixFormat");

  fmt_.channels = mix->nChannels;
  fmt_.sampleRate = mix->nSamplesPerSec;
  fmt_.bits = mix->wBitsPerSample;
  fmt_.isFloat = (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
  if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22)
  {
    auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix);
    fmt_.channelMask = ext->dwChannelMask;
    fmt_.isFloat = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
  }

  // Shared mode. Loopback streams cannot be event-driven, so they are polled instead.
  // periodicity must be 0 in shared mode.
  const DWORD flags = loopback_ ? AUDCLNT_STREAMFLAGS_LOOPBACK : AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
  HRESULT hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, bufferHns_, 0, mix, nullptr);
  CoTaskMemFree(mix);
  if (FAILED(hr)) { std::fprintf(stderr, "[WasapiCapture] Initialize failed: %s\n", HrStr(hr).c_str()); return false; }

  stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr); // manual-reset
  if (!stopEvent_) return false;
  if (!loopback_)
  {
    dataEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!dataEvent_) return false;
    HR_FAIL(client_->SetEventHandle(dataEvent_), "SetEventHandle");
  }
  HR_FAIL(client_->GetService(__uuidof(IAudioCaptureClient), &capture_), "GetService(IAudioCaptureClient)");

  std::printf("[WasapiCapture] %s: %u ch, %u Hz, %u-bit %s, mask=0x%X\n",
              loopback_ ? "loopback" : "capture", fmt_.channels, fmt_.sampleRate, fmt_.bits,
              fmt_.isFloat ? "float" : "int", fmt_.channelMask);
  return true;
}

bool WasapiCapture::Start()
{
  if (!ring_)
  {
    std::fprintf(stderr, "[WasapiCapture] SetRing() not called before Start()\n");
    return false;
  }
  if (running_.exchange(true))
    return true;
  ResetEvent(stopEvent_);
  HR_FAIL(client_->Start(), "client Start");
  thread_ = std::thread(&WasapiCapture::ThreadProc, this);
  return true;
}

void WasapiCapture::Stop()
{
  if (!running_.exchange(false))
    return;
  if (stopEvent_) SetEvent(stopEvent_);
  if (thread_.joinable()) thread_.join();
  if (client_) client_->Stop();
}

void WasapiCapture::ThreadProc()
{
  DWORD taskIndex = 0;
  HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

  const size_t bpf = fmt_.bytesPerFrame();

  auto drain = [&]() {
    UINT32 packet = 0;
    while (SUCCEEDED(capture_->GetNextPacketSize(&packet)) && packet > 0)
    {
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      if (FAILED(capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
        break;

      const size_t bytes = static_cast<size_t>(frames) * bpf;
      if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
      {
        // Push silence so the timeline stays continuous.
        static thread_local std::vector<uint8_t> zeros;
        if (zeros.size() < bytes) zeros.assign(bytes, 0);
        ring_->Write(zeros.data(), bytes);
      }
      else if (data)
      {
        ring_->Write(data, bytes); // overflow is dropped by the ring (consumer trims drift)
      }
      capture_->ReleaseBuffer(frames);
    }
  };

  if (loopback_)
  {
    // Loopback streams aren't event-driven: poll, waking early if asked to stop.
    const DWORD pollMs = (bufferHns_ <= 100000) ? 2 : 5;
    while (running_.load(std::memory_order_relaxed))
    {
      if (WaitForSingleObject(stopEvent_, pollMs) == WAIT_OBJECT_0)
        break;
      drain();
    }
  }
  else
  {
    HANDLE waits[2] = { dataEvent_, stopEvent_ };
    while (running_.load(std::memory_order_relaxed))
    {
      DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
      if (w == WAIT_OBJECT_0 + 1) // stop
        break;
      drain();
    }
  }

  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}
