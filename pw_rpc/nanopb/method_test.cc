// Copyright 2020 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

#include "pw_rpc/nanopb/internal/method.h"

#include <array>

#include "gtest/gtest.h"
#include "pw_rpc/nanopb/internal/method_union.h"
#include "pw_rpc/server_context.h"
#include "pw_rpc/service.h"
#include "pw_rpc_nanopb_private/internal_test_utils.h"
#include "pw_rpc_private/internal_test_utils.h"
#include "pw_rpc_private/method_impl_tester.h"
#include "pw_rpc_test_protos/test.pb.h"

namespace pw::rpc::internal {
namespace {

using std::byte;

struct FakePb {};

// Create a fake service for use with the MethodImplTester.
class TestNanopbService final : public Service {
 public:
  // Unary signatures

  Status Unary(ServerContext&, const FakePb&, FakePb&) { return Status(); }

  static Status StaticUnary(ServerContext&, const FakePb&, FakePb&) {
    return Status();
  }

  Status UnaryWrongArg(ServerContext&, FakePb&, FakePb&) { return Status(); }

  static void StaticUnaryVoidReturn(ServerContext&, const FakePb&, FakePb&) {}

  // Server streaming signatures

  void ServerStreaming(ServerContext&,
                       const FakePb&,
                       NanopbServerWriter<FakePb>&) {}

  static void StaticServerStreaming(ServerContext&,
                                    const FakePb&,
                                    NanopbServerWriter<FakePb>&) {}

  int ServerStreamingBadReturn(ServerContext&,
                               const FakePb&,
                               NanopbServerWriter<FakePb>&) {
    return 5;
  }

  static void StaticServerStreamingMissingArg(const FakePb&,
                                              NanopbServerWriter<FakePb>&) {}

  // Client streaming signatures

  void ClientStreaming(ServerContext&, NanopbServerReader<FakePb, FakePb>&) {}

  static void StaticClientStreaming(ServerContext&,
                                    NanopbServerReader<FakePb, FakePb>&) {}

  int ClientStreamingBadReturn(ServerContext&,
                               NanopbServerReader<FakePb, FakePb>&) {
    return 0;
  }

  static void StaticClientStreamingMissingArg(
      NanopbServerReader<FakePb, FakePb>&) {}

  // Bidirectional streaming signatures

  void BidirectionalStreaming(ServerContext&,
                              NanopbServerReaderWriter<FakePb, FakePb>&) {}

  static void StaticBidirectionalStreaming(
      ServerContext&, NanopbServerReaderWriter<FakePb, FakePb>&) {}

  int BidirectionalStreamingBadReturn(
      ServerContext&, NanopbServerReaderWriter<FakePb, FakePb>&) {
    return 0;
  }

  static void StaticBidirectionalStreamingMissingArg(
      NanopbServerReaderWriter<FakePb, FakePb>&) {}
};

struct WrongPb;

// Test matches() rejects incorrect request/response types.
// clang-format off
static_assert(!NanopbMethod::template matches<&TestNanopbService::Unary, WrongPb, FakePb>());
static_assert(!NanopbMethod::template matches<&TestNanopbService::Unary, FakePb, WrongPb>());
static_assert(!NanopbMethod::template matches<&TestNanopbService::Unary, WrongPb, WrongPb>());
static_assert(!NanopbMethod::template matches<&TestNanopbService::StaticUnary, FakePb, WrongPb>());

static_assert(!NanopbMethod::template matches<&TestNanopbService::ServerStreaming, WrongPb, FakePb>());
static_assert(!NanopbMethod::template matches<&TestNanopbService::StaticServerStreaming, FakePb, WrongPb>());

static_assert(!NanopbMethod::template matches<&TestNanopbService::ClientStreaming, WrongPb, FakePb>());
static_assert(!NanopbMethod::template matches<&TestNanopbService::StaticClientStreaming, FakePb, WrongPb>());

static_assert(!NanopbMethod::template matches<&TestNanopbService::BidirectionalStreaming, WrongPb, FakePb>());
static_assert(!NanopbMethod::template matches<&TestNanopbService::StaticBidirectionalStreaming, FakePb, WrongPb>());
// clang-format on

static_assert(MethodImplTests<NanopbMethod, TestNanopbService>().Pass(
    MatchesTypes<FakePb, FakePb>(), CreationArgs<nullptr, nullptr>()));

pw_rpc_test_TestRequest last_request;
ServerWriter<pw_rpc_test_TestResponse> last_writer;

Status AddFive(ServerContext&,
               const pw_rpc_test_TestRequest& request,
               pw_rpc_test_TestResponse& response) {
  last_request = request;
  response.value = request.integer + 5;
  return Status::Unauthenticated();
}

Status DoNothing(ServerContext&, const pw_rpc_test_Empty&, pw_rpc_test_Empty&) {
  return Status::Unknown();
}

void StartStream(ServerContext&,
                 const pw_rpc_test_TestRequest& request,
                 ServerWriter<pw_rpc_test_TestResponse>& writer) {
  last_request = request;
  last_writer = std::move(writer);
}

class FakeService : public Service {
 public:
  FakeService(uint32_t id) : Service(id, kMethods) {}

  static constexpr std::array<NanopbMethodUnion, 3> kMethods = {
      NanopbMethod::Unary<DoNothing>(
          10u, pw_rpc_test_Empty_fields, pw_rpc_test_Empty_fields),
      NanopbMethod::Unary<AddFive>(
          11u, pw_rpc_test_TestRequest_fields, pw_rpc_test_TestResponse_fields),
      NanopbMethod::ServerStreaming<StartStream>(
          12u, pw_rpc_test_TestRequest_fields, pw_rpc_test_TestResponse_fields),
  };
};

TEST(NanopbMethod, UnaryRpc_SendsResponse) {
  PW_ENCODE_PB(
      pw_rpc_test_TestRequest, request, .integer = 123, .status_code = 0);

  const NanopbMethod& method =
      std::get<1>(FakeService::kMethods).nanopb_method();
  ServerContextForTest<FakeService> context(method);
  method.Invoke(context.get(), context.request(request));

  const Packet& response = context.output().sent_packet();
  EXPECT_EQ(response.status(), Status::Unauthenticated());

  // Field 1 (encoded as 1 << 3) with 128 as the value.
  constexpr std::byte expected[]{
      std::byte{0x08}, std::byte{0x80}, std::byte{0x01}};

  EXPECT_EQ(sizeof(expected), response.payload().size());
  EXPECT_EQ(0,
            std::memcmp(expected, response.payload().data(), sizeof(expected)));

  EXPECT_EQ(123, last_request.integer);
}

TEST(NanopbMethod, UnaryRpc_InvalidPayload_SendsError) {
  std::array<byte, 8> bad_payload{byte{0xFF}, byte{0xAA}, byte{0xDD}};

  const NanopbMethod& method =
      std::get<0>(FakeService::kMethods).nanopb_method();
  ServerContextForTest<FakeService> context(method);
  method.Invoke(context.get(), context.request(bad_payload));

  const Packet& packet = context.output().sent_packet();
  EXPECT_EQ(PacketType::SERVER_ERROR, packet.type());
  EXPECT_EQ(Status::DataLoss(), packet.status());
  EXPECT_EQ(context.service_id(), packet.service_id());
  EXPECT_EQ(method.id(), packet.method_id());
}

TEST(NanopbMethod, UnaryRpc_BufferTooSmallForResponse_SendsInternalError) {
  constexpr int64_t value = 0x7FFFFFFF'FFFFFF00ll;
  PW_ENCODE_PB(
      pw_rpc_test_TestRequest, request, .integer = value, .status_code = 0);

  const NanopbMethod& method =
      std::get<1>(FakeService::kMethods).nanopb_method();
  // Output buffer is too small for the response, but can fit an error packet.
  ServerContextForTest<FakeService, 22> context(method);
  ASSERT_LT(
      context.output().buffer_size(),
      context.request(request).MinEncodedSizeBytes() + request.size() + 1);

  method.Invoke(context.get(), context.request(request));

  const Packet& packet = context.output().sent_packet();
  EXPECT_EQ(PacketType::SERVER_ERROR, packet.type());
  EXPECT_EQ(Status::Internal(), packet.status());
  EXPECT_EQ(context.service_id(), packet.service_id());
  EXPECT_EQ(method.id(), packet.method_id());

  EXPECT_EQ(value, last_request.integer);
}

TEST(NanopbMethod, ServerStreamingRpc_SendsNothingWhenInitiallyCalled) {
  PW_ENCODE_PB(
      pw_rpc_test_TestRequest, request, .integer = 555, .status_code = 0);

  const NanopbMethod& method =
      std::get<2>(FakeService::kMethods).nanopb_method();
  ServerContextForTest<FakeService> context(method);

  method.Invoke(context.get(), context.request(request));

  EXPECT_EQ(0u, context.output().packet_count());
  EXPECT_EQ(555, last_request.integer);
}

TEST(NanopbMethod, ServerWriter_SendsResponse) {
  const NanopbMethod& method =
      std::get<2>(FakeService::kMethods).nanopb_method();
  ServerContextForTest<FakeService> context(method);

  method.Invoke(context.get(), context.request({}));

  EXPECT_EQ(OkStatus(), last_writer.Write({.value = 100}));

  PW_ENCODE_PB(pw_rpc_test_TestResponse, payload, .value = 100);
  std::array<byte, 128> encoded_response = {};
  auto encoded = context.server_stream(payload).Encode(encoded_response);
  ASSERT_EQ(OkStatus(), encoded.status());

  ASSERT_EQ(encoded.value().size(), context.output().sent_data().size());
  EXPECT_EQ(0,
            std::memcmp(encoded.value().data(),
                        context.output().sent_data().data(),
                        encoded.value().size()));
}

TEST(NanopbMethod, ServerWriter_WriteWhenClosed_ReturnsFailedPrecondition) {
  const NanopbMethod& method =
      std::get<2>(FakeService::kMethods).nanopb_method();
  ServerContextForTest<FakeService> context(method);

  method.Invoke(context.get(), context.request({}));

  EXPECT_EQ(OkStatus(), last_writer.Finish());
  EXPECT_TRUE(last_writer.Write({.value = 100}).IsFailedPrecondition());
}

TEST(NanopbMethod, ServerWriter_WriteAfterMoved_ReturnsFailedPrecondition) {
  const NanopbMethod& method =
      std::get<2>(FakeService::kMethods).nanopb_method();
  ServerContextForTest<FakeService> context(method);

  method.Invoke(context.get(), context.request({}));
  ServerWriter<pw_rpc_test_TestResponse> new_writer = std::move(last_writer);

  EXPECT_EQ(OkStatus(), new_writer.Write({.value = 100}));

  EXPECT_EQ(Status::FailedPrecondition(), last_writer.Write({.value = 100}));
  EXPECT_EQ(Status::FailedPrecondition(), last_writer.Finish());

  EXPECT_EQ(OkStatus(), new_writer.Finish());
}

TEST(NanopbMethod,
     ServerStreamingRpc_ServerWriterBufferTooSmall_InternalError) {
  const NanopbMethod& method =
      std::get<2>(FakeService::kMethods).nanopb_method();

  constexpr size_t kNoPayloadPacketSize =
      2 /* type */ + 2 /* channel */ + 5 /* service */ + 5 /* method */ +
      0 /* payload (when empty) */ + 0 /* status (when OK)*/;

  // Make the buffer barely fit a packet with no payload.
  ServerContextForTest<FakeService, kNoPayloadPacketSize> context(method);

  // Verify that the encoded size of a packet with an empty payload is correct.
  std::array<byte, 128> encoded_response = {};
  auto encoded = context.request({}).Encode(encoded_response);
  ASSERT_EQ(OkStatus(), encoded.status());
  ASSERT_EQ(kNoPayloadPacketSize, encoded.value().size());

  method.Invoke(context.get(), context.request({}));

  EXPECT_EQ(OkStatus(), last_writer.Write({}));  // Barely fits
  EXPECT_EQ(Status::Internal(), last_writer.Write({.value = 1}));  // Too big
}

// TODO(pwbug/428): Test NanopbServerReader / NanopbServerReaderWriter. In
//     particular, test that the client stream callback correctly decodes
//     incoming messages with Nanopb.

}  // namespace
}  // namespace pw::rpc::internal
