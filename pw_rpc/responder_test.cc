// Copyright 2021 The Pigweed Authors
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

#include "pw_rpc/internal/responder.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include "gtest/gtest.h"
#include "pw_rpc/internal/test_method.h"
#include "pw_rpc/server_context.h"
#include "pw_rpc/service.h"
#include "pw_rpc_private/fake_server_reader_writer.h"
#include "pw_rpc_private/internal_test_utils.h"

namespace pw::rpc {

class TestService : public Service {
 public:
  constexpr TestService(uint32_t id) : Service(id, method) {}

  static constexpr internal::TestMethodUnion method = internal::TestMethod(8);
};

namespace internal {
namespace {

using pw::rpc::internal::test::FakeServerWriter;
using std::byte;

TEST(ServerWriter, ConstructWithContext_StartsOpen) {
  ServerContextForTest<TestService> context(TestService::method.method());

  FakeServerWriter writer(context.get());

  EXPECT_TRUE(writer.open());
}

TEST(ServerWriter, Move_ClosesOriginal) {
  ServerContextForTest<TestService> context(TestService::method.method());

  FakeServerWriter moved(context.get());
  FakeServerWriter writer(std::move(moved));

#ifndef __clang_analyzer__
  EXPECT_FALSE(moved.open());
#endif  // ignore use-after-move
  EXPECT_TRUE(writer.open());
}

TEST(ServerWriter, DefaultConstruct_Closed) {
  FakeServerWriter writer;

  EXPECT_FALSE(writer.open());
}

TEST(ServerWriter, Construct_RegistersWithServer) {
  ServerContextForTest<TestService> context(TestService::method.method());
  FakeServerWriter writer(context.get());

  auto& writers = context.server().writers();
  EXPECT_FALSE(writers.empty());
  auto it = std::find_if(writers.begin(), writers.end(), [&](auto& w) {
    return &w == &writer.as_responder();
  });
  ASSERT_NE(it, writers.end());
}

TEST(ServerWriter, Destruct_RemovesFromServer) {
  ServerContextForTest<TestService> context(TestService::method.method());
  { FakeServerWriter writer(context.get()); }

  auto& writers = context.server().writers();
  EXPECT_TRUE(writers.empty());
}

TEST(ServerWriter, Finish_RemovesFromServer) {
  ServerContextForTest<TestService> context(TestService::method.method());
  FakeServerWriter writer(context.get());

  EXPECT_EQ(OkStatus(), writer.Finish());

  auto& writers = context.server().writers();
  EXPECT_TRUE(writers.empty());
}

TEST(ServerWriter, Finish_SendsResponse) {
  ServerContextForTest<TestService> context(TestService::method.method());
  FakeServerWriter writer(context.get());

  EXPECT_EQ(OkStatus(), writer.Finish());

  const Packet& packet = context.output().sent_packet();
  EXPECT_EQ(packet.type(), PacketType::RESPONSE);
  EXPECT_EQ(packet.channel_id(), context.channel_id());
  EXPECT_EQ(packet.service_id(), context.service_id());
  EXPECT_EQ(packet.method_id(), context.get().method().id());
  EXPECT_TRUE(packet.payload().empty());
  EXPECT_EQ(packet.status(), OkStatus());
}

TEST(ServerWriter, Finish_ReturnsStatusFromChannelSend) {
  ServerContextForTest<TestService> context(TestService::method.method());
  FakeServerWriter writer(context.get());
  context.output().set_send_status(Status::Unauthenticated());

  EXPECT_EQ(Status::Unauthenticated(), writer.Finish());
}

TEST(ServerWriter, Close) {
  ServerContextForTest<TestService> context(TestService::method.method());
  FakeServerWriter writer(context.get());

  ASSERT_TRUE(writer.open());
  EXPECT_EQ(OkStatus(), writer.Finish());
  EXPECT_FALSE(writer.open());
  EXPECT_EQ(Status::FailedPrecondition(), writer.Finish());
}

TEST(ServerWriter, Close_ReleasesBuffer) {
  ServerContextForTest<TestService> context(TestService::method.method());
  FakeServerWriter writer(context.get());

  ASSERT_TRUE(writer.open());
  auto buffer = writer.PayloadBuffer();
  buffer[0] = std::byte{0};
  EXPECT_FALSE(writer.output_buffer().empty());
  EXPECT_EQ(OkStatus(), writer.Finish());
  EXPECT_FALSE(writer.open());
  EXPECT_TRUE(writer.output_buffer().empty());
}

TEST(ServerWriter, Open_SendsPacketWithPayload) {
  ServerContextForTest<TestService> context(TestService::method.method());
  FakeServerWriter writer(context.get());

  constexpr byte data[] = {byte{0xf0}, byte{0x0d}};
  ASSERT_EQ(OkStatus(), writer.Write(data));

  byte encoded[64];
  auto result = context.server_stream(data).Encode(encoded);
  ASSERT_EQ(OkStatus(), result.status());

  EXPECT_EQ(result.value().size(), context.output().sent_data().size());
  EXPECT_EQ(
      0,
      std::memcmp(
          encoded, context.output().sent_data().data(), result.value().size()));
}

TEST(ServerWriter, Closed_IgnoresFinish) {
  ServerContextForTest<TestService> context(TestService::method.method());
  FakeServerWriter writer(context.get());

  EXPECT_EQ(OkStatus(), writer.Finish());
  EXPECT_EQ(Status::FailedPrecondition(), writer.Finish());
}

TEST(ServerWriter, DefaultConstructor_NoClientStream) {
  FakeServerWriter writer;
  EXPECT_FALSE(writer.as_responder().has_client_stream());
  EXPECT_FALSE(writer.as_responder().client_stream_open());
}

TEST(ServerWriter, Open_NoClientStream) {
  ServerContextForTest<TestService> context(TestService::method.method());
  FakeServerWriter writer(context.get());

  EXPECT_FALSE(writer.as_responder().has_client_stream());
  EXPECT_FALSE(writer.as_responder().client_stream_open());
}

TEST(ServerReader, DefaultConstructor_ClientStreamClosed) {
  test::FakeServerReader reader;
  EXPECT_TRUE(reader.as_responder().has_client_stream());
  EXPECT_FALSE(reader.as_responder().client_stream_open());
}

TEST(ServerReader, Open_ClientStreamStartsOpen) {
  ServerContextForTest<TestService> context(TestService::method.method());
  test::FakeServerReader reader(context.get());

  EXPECT_TRUE(reader.as_responder().has_client_stream());
  EXPECT_TRUE(reader.as_responder().client_stream_open());
}

TEST(ServerReader, Close_ClosesClientStream) {
  ServerContextForTest<TestService> context(TestService::method.method());
  test::FakeServerReader reader(context.get());

  EXPECT_TRUE(reader.as_responder().open());
  EXPECT_TRUE(reader.as_responder().client_stream_open());
  EXPECT_EQ(OkStatus(), reader.as_responder().CloseAndSendResponse(OkStatus()));

  EXPECT_FALSE(reader.as_responder().open());
  EXPECT_FALSE(reader.as_responder().client_stream_open());
}

TEST(ServerReader, HandleClientStream_OnlyClosesClientStream) {
  ServerContextForTest<TestService> context(TestService::method.method());
  test::FakeServerReader reader(context.get());

  EXPECT_TRUE(reader.open());
  EXPECT_TRUE(reader.as_responder().client_stream_open());
  reader.as_responder().EndClientStream();

  EXPECT_TRUE(reader.open());
  EXPECT_FALSE(reader.as_responder().client_stream_open());
}

TEST(ServerReaderWriter, Move_MaintainsClientStream) {
  ServerContextForTest<TestService> context(TestService::method.method());
  test::FakeServerReaderWriter reader_writer(context.get());
  test::FakeServerReaderWriter destination;

  EXPECT_FALSE(destination.as_responder().client_stream_open());

  destination = std::move(reader_writer);
  EXPECT_TRUE(destination.as_responder().has_client_stream());
  EXPECT_TRUE(destination.as_responder().client_stream_open());
}

TEST(ServerReaderWriter, Move_MovesCallbacks) {
  ServerContextForTest<TestService> context(TestService::method.method());
  test::FakeServerReaderWriter reader_writer(context.get());

  int calls = 0;
  reader_writer.set_on_error([&calls](Status) { calls += 1; });
  reader_writer.set_on_next([&calls](ConstByteSpan) { calls += 1; });

#if PW_RPC_CLIENT_STREAM_END_CALLBACK
  reader_writer.set_on_client_stream_end([&calls]() { calls += 1; });
#endif  // PW_RPC_CLIENT_STREAM_END_CALLBACK

  test::FakeServerReaderWriter destination(std::move(reader_writer));
  destination.as_responder().HandleClientStream({});
  destination.as_responder().EndClientStream();
  destination.as_responder().HandleError(Status::Unknown());

  EXPECT_EQ(calls, 2 + PW_RPC_CLIENT_STREAM_END_CALLBACK);
}

}  // namespace
}  // namespace internal
}  // namespace pw::rpc
