// test_encoder.cpp — contract tests for SpdifEncoder (PCM -> AC3 -> IEC 61937).
#include "doctest.h"
#include "SpdifEncoder.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

#include <cstdint>
#include <vector>

namespace {

// IEC 61937 burst preamble Pa=0xF872, Pb=0x4E1F. Accept either byte order, since the
// only thing that matters is that the muxer produced a real burst (Phase 1 already
// proved ffmpeg can demux/decode it back to AC3).
bool HasIec61937Sync(const uint8_t* b)
{
  const bool le = (b[0] == 0x72 && b[1] == 0xF8 && b[2] == 0x1F && b[3] == 0x4E);
  const bool be = (b[0] == 0xF8 && b[1] == 0x72 && b[2] == 0x4E && b[3] == 0x1F);
  return le || be;
}

// DTS frame syncword: 0x7FFE8001 (16-bit core) in BE/LE, or 14-bit core in BE/LE.
// When DTS is encoded at full rate (1536 kbps), the 2048-byte DTS payload completely fills
// the S/PDIF slot, so the IEC 61937 preamble is omitted by FFmpeg and the burst begins
// directly with the DTS syncword.
bool HasDtsSync(const uint8_t* b)
{
  const bool be16 = (b[0] == 0x7F && b[1] == 0xFE && b[2] == 0x80 && b[3] == 0x01);
  const bool le16 = (b[0] == 0xFE && b[1] == 0x7F && b[2] == 0x01 && b[3] == 0x80);
  const bool be14 = (b[0] == 0x1F && b[1] == 0xFE && b[2] == 0x80 && b[3] == 0x00);
  const bool le14 = (b[0] == 0xFF && b[1] == 0x1F && b[2] == 0x00 && b[3] == 0xE8);
  return be16 || le16 || be14 || le14;
}

// Helper: init an encoder for `channels` interleaved samples in `fmt`.
bool InitFor(SpdifEncoder& enc, int channels, AVSampleFormat fmt, int rate = 48000,
             SpdifEncoder::Codec codec = SpdifEncoder::Codec::AC3,
             int64_t dtsBitRate = 768000)
{
  SpdifEncoder::Params p;
  p.codec = codec;
  p.sampleRate = rate;
  p.dtsBitRate = dtsBitRate;
  p.inSampleFmt = fmt;
  av_channel_layout_default(&p.inLayout, channels);
  bool ok = enc.Init(p);
  av_channel_layout_uninit(&p.inLayout);
  return ok;
}

} // namespace

TEST_CASE("init: 5.1 float")
{
  SpdifEncoder enc;
  REQUIRE(InitFor(enc, 6, AV_SAMPLE_FMT_FLT));
  CHECK(enc.IsOpen());
  CHECK(enc.FramesPerPacket() == 1536);
  CHECK(enc.InChannels() == 6);
}

TEST_CASE("encode: 5.1 float -> 6144-byte IEC 61937 burst")
{
  SpdifEncoder enc;
  REQUIRE(InitFor(enc, 6, AV_SAMPLE_FMT_FLT));
  std::vector<float> in(static_cast<size_t>(enc.FramesPerPacket()) * 6, 0.25f);
  std::vector<uint8_t> out(SpdifEncoder::kMaxBytesPerPacket);
  int n = enc.EncodePacket(reinterpret_cast<uint8_t*>(in.data()), out.data(),
                           static_cast<int>(out.size()));
  REQUIRE(n > 0);
  CHECK(n == 6144);
  CHECK(HasIec61937Sync(out.data()));
}

TEST_CASE("encode: 7.1 input is downmixed to 5.1")
{
  SpdifEncoder enc;
  REQUIRE(InitFor(enc, 8, AV_SAMPLE_FMT_FLT));
  CHECK(enc.InChannels() == 8);
  std::vector<float> in(static_cast<size_t>(enc.FramesPerPacket()) * 8, 0.1f);
  std::vector<uint8_t> out(SpdifEncoder::kMaxBytesPerPacket);
  int n = enc.EncodePacket(reinterpret_cast<uint8_t*>(in.data()), out.data(),
                           static_cast<int>(out.size()));
  CHECK(n > 0);
  CHECK(HasIec61937Sync(out.data()));
}

TEST_CASE("encode: stereo input is accepted")
{
  SpdifEncoder enc;
  REQUIRE(InitFor(enc, 2, AV_SAMPLE_FMT_FLT));
  std::vector<float> in(static_cast<size_t>(enc.FramesPerPacket()) * 2, 0.1f);
  std::vector<uint8_t> out(SpdifEncoder::kMaxBytesPerPacket);
  CHECK(enc.EncodePacket(reinterpret_cast<uint8_t*>(in.data()), out.data(),
                         static_cast<int>(out.size())) > 0);
}

TEST_CASE("encode: 16-bit integer input is accepted")
{
  SpdifEncoder enc;
  REQUIRE(InitFor(enc, 6, AV_SAMPLE_FMT_S16));
  std::vector<int16_t> in(static_cast<size_t>(enc.FramesPerPacket()) * 6, 1000);
  std::vector<uint8_t> out(SpdifEncoder::kMaxBytesPerPacket);
  int n = enc.EncodePacket(reinterpret_cast<uint8_t*>(in.data()), out.data(),
                           static_cast<int>(out.size()));
  CHECK(n > 0);
  CHECK(HasIec61937Sync(out.data()));
}

TEST_CASE("encode: consecutive packets keep producing bursts")
{
  SpdifEncoder enc;
  REQUIRE(InitFor(enc, 6, AV_SAMPLE_FMT_FLT));
  std::vector<float> in(static_cast<size_t>(enc.FramesPerPacket()) * 6, 0.2f);
  std::vector<uint8_t> out(SpdifEncoder::kMaxBytesPerPacket);
  int produced = 0;
  for (int i = 0; i < 10; ++i)
    if (enc.EncodePacket(reinterpret_cast<uint8_t*>(in.data()), out.data(),
                         static_cast<int>(out.size())) == 6144)
      ++produced;
  CHECK(produced >= 9); // allow at most one priming call to yield nothing
}

TEST_CASE("init: AC3-invalid sample rate fails cleanly")
{
  SpdifEncoder enc;
  CHECK_FALSE(InitFor(enc, 6, AV_SAMPLE_FMT_FLT, 96000)); // AC3 supports 48/44.1/32k only
}

TEST_CASE("init: DTS 5.1 encoder (full rate 1536k and half rate 768k)")
{
  SpdifEncoder encFull;
  REQUIRE(InitFor(encFull, 6, AV_SAMPLE_FMT_FLT, 48000, SpdifEncoder::Codec::DTS, 1536000));
  CHECK(encFull.IsOpen());
  CHECK(encFull.FramesPerPacket() == 512);
  CHECK(encFull.InChannels() == 6);

  SpdifEncoder encHalf;
  REQUIRE(InitFor(encHalf, 6, AV_SAMPLE_FMT_FLT, 48000, SpdifEncoder::Codec::DTS, 768000));
  CHECK(encHalf.IsOpen());
  CHECK(encHalf.FramesPerPacket() == 512);
  CHECK(encHalf.InChannels() == 6);
}

TEST_CASE("DTS 5.1 -> 2048-byte IEC 61937 burst")
{
  SpdifEncoder enc;
  REQUIRE(InitFor(enc, 6, AV_SAMPLE_FMT_FLT, 48000, SpdifEncoder::Codec::DTS));
  std::vector<float> in(static_cast<size_t>(enc.FramesPerPacket()) * 6, 0.1f);
  std::vector<uint8_t> out(SpdifEncoder::kMaxBytesPerPacket);

  int n = enc.EncodePacket(reinterpret_cast<const uint8_t*>(in.data()), out.data(),
                           static_cast<int>(out.size()));
  REQUIRE(n > 0);
  CHECK(n == 2048);
  CHECK(HasIec61937Sync(out.data()));
}

TEST_CASE("DTS 5.1 -> 2048-byte burst (full rate 1536k)")
{
  SpdifEncoder enc;
  REQUIRE(InitFor(enc, 6, AV_SAMPLE_FMT_FLT, 48000, SpdifEncoder::Codec::DTS, 1536000));
  std::vector<float> in(static_cast<size_t>(enc.FramesPerPacket()) * 6, 0.1f);
  std::vector<uint8_t> out(SpdifEncoder::kMaxBytesPerPacket);

  int n = enc.EncodePacket(reinterpret_cast<const uint8_t*>(in.data()), out.data(),
                           static_cast<int>(out.size()));
  REQUIRE(n > 0);
  CHECK(n == 2048);
  CHECK((HasIec61937Sync(out.data()) || HasDtsSync(out.data())));
}

TEST_CASE("DTS 5.1 -> 2048-byte IEC 61937 burst (half rate 768k)")
{
  SpdifEncoder enc;
  REQUIRE(InitFor(enc, 6, AV_SAMPLE_FMT_FLT, 48000, SpdifEncoder::Codec::DTS, 768000));
  std::vector<float> in(static_cast<size_t>(enc.FramesPerPacket()) * 6, 0.1f);
  std::vector<uint8_t> out(SpdifEncoder::kMaxBytesPerPacket);

  int n = enc.EncodePacket(reinterpret_cast<const uint8_t*>(in.data()), out.data(),
                           static_cast<int>(out.size()));
  REQUIRE(n > 0);
  CHECK(n == 2048);
  CHECK(HasIec61937Sync(out.data()));
}

// (The surround upmix is covered by a meaningful behavioral test in test_upmix.cpp:
//  it decodes the AC3 output and checks that energy actually lands in the surround channels.)
