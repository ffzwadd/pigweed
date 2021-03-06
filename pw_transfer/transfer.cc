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

#include "pw_transfer/transfer.h"

#include "pw_assert/check.h"
#include "pw_log/log.h"
#include "pw_status/try.h"
#include "pw_transfer/transfer.pwpb.h"
#include "pw_transfer_private/chunk.h"
#include "pw_varint/varint.h"

namespace pw::transfer {

void TransferService::Read(ServerContext&,
                           RawServerReaderWriter& reader_writer) {
  read_stream_ = std::move(reader_writer);

  read_stream_.set_on_next(
      [this](ConstByteSpan message) { OnReadMessage(message); });
}

void TransferService::Write(ServerContext&,
                            RawServerReaderWriter& reader_writer) {
  write_stream_ = std::move(reader_writer);

  write_stream_.set_on_next(
      [this](ConstByteSpan message) { OnWriteMessage(message); });
}

void TransferService::SendStatusChunk(RawServerReaderWriter& stream,
                                      uint32_t transfer_id,
                                      Status status) {
  internal::Chunk chunk = {};
  chunk.transfer_id = transfer_id;
  chunk.status = status.code();

  Result<ConstByteSpan> result =
      internal::EncodeChunk(chunk, stream.PayloadBuffer());
  if (result.ok()) {
    stream.Write(result.value())
        .IgnoreError();  // TODO(pwbug/387): Handle Status properly
  }
}

bool TransferService::SendNextReadChunk(internal::Context& context) {
  if (context.pending_bytes() == 0) {
    return false;
  }

  ByteSpan buffer = read_stream_.PayloadBuffer();

  // Begin by doing a partial encode of all the metadata fields, leaving the
  // buffer with usable space for the chunk data at the end.
  Chunk::MemoryEncoder encoder(buffer);
  encoder.WriteTransferId(context.transfer_id())
      .IgnoreError();  // TODO(pwbug/387): Handle Status properly
  encoder.WriteOffset(context.offset())
      .IgnoreError();  // TODO(pwbug/387): Handle Status properly

  // Reserve space for the data proto field overhead and use the remainder of
  // the buffer for the chunk data.
  size_t reserved_size = encoder.size() + 1 /* data key */ + 5 /* data size */;

  ByteSpan data_buffer = buffer.subspan(reserved_size);
  size_t max_bytes_to_send =
      std::min(context.pending_bytes(), context.max_chunk_size_bytes());

  if (max_bytes_to_send < data_buffer.size()) {
    data_buffer = data_buffer.first(max_bytes_to_send);
  }

  Result<ByteSpan> data = context.reader().Read(data_buffer);
  if (data.status().IsOutOfRange()) {
    // No more data to read.
    encoder.WriteRemainingBytes(0)
        .IgnoreError();  // TODO(pwbug/387): Handle Status properly
    context.set_pending_bytes(0);
  } else if (!data.ok()) {
    read_stream_.ReleaseBuffer();
    return false;
  } else {
    encoder.WriteData(data.value())
        .IgnoreError();  // TODO(pwbug/387): Handle Status properly
    context.set_offset(context.offset() + data.value().size());
    context.set_pending_bytes(context.pending_bytes() - data.value().size());
  }

  return read_stream_.Write(encoder).ok();
}

void TransferService::OnReadMessage(ConstByteSpan message) {
  // All incoming chunks in a client read transfer are transfer parameter
  // updates, except for the final chunk, which is an acknowledgement of
  // completion.
  //
  // Transfer parameters may contain the following fields:
  //
  //   - transfer_id (required)
  //   - pending_bytes (required)
  //   - offset (required)
  //   - max_chunk_size_bytes
  //   - min_delay_microseconds (not yet supported)
  //
  internal::Chunk parameters;

  if (Status status = internal::DecodeChunk(message, parameters);
      !status.ok()) {
    // No special handling required here. The client will retransmit the chunk
    // when no response is received.
    PW_LOG_ERROR("Failed to decode incoming read transfer chunk");
    return;
  }

  Result<internal::Context*> result =
      read_transfers_.GetOrStartTransfer(parameters.transfer_id);
  if (!result.ok()) {
    PW_LOG_ERROR("Error handling read transfer %u: %d",
                 static_cast<unsigned>(parameters.transfer_id),
                 static_cast<int>(result.status().code()));
    SendStatusChunk(read_stream_, parameters.transfer_id, result.status());
    return;
  }

  internal::Context& transfer = *result.value();

  if (parameters.status.has_value()) {
    // Transfer has been terminated (successfully or not).
    Status status = parameters.status.value();
    if (!status.ok()) {
      PW_LOG_ERROR("Transfer %u failed with status %d",
                   static_cast<unsigned>(parameters.transfer_id),
                   static_cast<int>(status.code()));
    }
    transfer.Finish(status);
    return;
  }

  if (!parameters.pending_bytes.has_value()) {
    // Malformed chunk.
    SendStatusChunk(
        read_stream_, parameters.transfer_id, Status::InvalidArgument());
    transfer.Finish(Status::InvalidArgument());
    return;
  }

  // Update local transfer fields based on the received chunk.
  if (transfer.offset() != parameters.offset) {
    // TODO(frolv): pw_stream does not yet support seeking, so this temporarily
    // cancels the transfer. Once seeking is added, this should be updated.
    //
    //   transfer.set_offset(parameters.offset.value());
    //   transfer.Seek(transfer.offset());
    //
    SendStatusChunk(
        read_stream_, parameters.transfer_id, Status::Unimplemented());
    transfer.Finish(Status::Unimplemented());
    return;
  }

  if (parameters.max_chunk_size_bytes.has_value()) {
    transfer.set_max_chunk_size_bytes(
        std::min(static_cast<size_t>(parameters.max_chunk_size_bytes.value()),
                 max_chunk_size_bytes_));
  }

  transfer.set_pending_bytes(parameters.pending_bytes.value());
  while (SendNextReadChunk(transfer)) {
    // Empty.
  }
}

void TransferService::OnWriteMessage(ConstByteSpan message) {
  // Process an incoming chunk during a client write transfer. The chunk may
  // either be the initial "start write" chunk (which only contains the transfer
  // ID), or a data chunk.
  internal::Chunk chunk;

  if (Status status = internal::DecodeChunk(message, chunk); !status.ok()) {
    PW_LOG_ERROR("Failed to decode incoming write transfer chunk");
    return;
  }

  // Try to find an active write transfer for the requested ID, or start a new
  // one if a writable TransferHandler is registered for it.
  Result<internal::Context*> maybe_context =
      write_transfers_.GetOrStartTransfer(chunk.transfer_id);
  if (!maybe_context.ok()) {
    PW_LOG_ERROR("Error handling write transfer %u: %d",
                 static_cast<unsigned>(chunk.transfer_id),
                 static_cast<int>(maybe_context.status().code()));
    SendStatusChunk(write_stream_, chunk.transfer_id, maybe_context.status());
    return;
  }

  internal::Context& transfer = *maybe_context.value();

  // Check for a client-side error terminating the transfer.
  if (chunk.status.has_value()) {
    transfer.Finish(chunk.status.value());
    return;
  }

  // Copy data from the chunk into the transfer handler's Writer, if it is at
  // the offset the transfer is currently expecting. Under some circumstances,
  // the chunk's data may be empty (e.g. a zero-length transfer). In that case,
  // handle the chunk as if the data exists.
  bool chunk_data_processed = false;

  if (chunk.offset == transfer.offset()) {
    if (chunk.data.empty()) {
      chunk_data_processed = true;
    } else if (chunk.data.size() <= transfer.pending_bytes()) {
      if (Status status = transfer.writer().Write(chunk.data); !status.ok()) {
        SendStatusChunk(write_stream_, chunk.transfer_id, status);
        transfer.Finish(status);
        return;
      }
      transfer.set_offset(transfer.offset() + chunk.data.size());
      transfer.set_pending_bytes(transfer.pending_bytes() - chunk.data.size());
      chunk_data_processed = true;
    }
  } else {
    // Bad offset; reset pending_bytes to send another parameters chunk.
    transfer.set_pending_bytes(0);
  }

  // When the client sets remaining_bytes to 0, it indicates completion of the
  // transfer. Acknowledge the completion through a status chunk and clean up.
  if (chunk_data_processed && chunk.remaining_bytes == 0) {
    SendStatusChunk(write_stream_, chunk.transfer_id, OkStatus());
    transfer.Finish(OkStatus());
    return;
  }

  if (transfer.pending_bytes() > 0) {
    // Expecting more data to be sent by the client. Wait for the next chunk.
    return;
  }

  // All pending data has been received. Send a new parameters chunk to start
  // the next batch.
  transfer.set_pending_bytes(
      std::min(default_max_bytes_to_receive_,
               transfer.writer().ConservativeWriteLimit()));

  internal::Chunk parameters = {};
  parameters.transfer_id = transfer.transfer_id();
  parameters.offset = transfer.offset();
  parameters.pending_bytes = transfer.pending_bytes();
  parameters.max_chunk_size_bytes = MaxWriteChunkSize(transfer);

  if (auto data =
          internal::EncodeChunk(parameters, write_stream_.PayloadBuffer());
      data.ok()) {
    write_stream_.Write(*data);
  }
}

// Calculates the maximum size of actual data that can be sent within a single
// client write transfer chunk, accounting for the overhead of the transfer
// protocol and RPC system.
//
// Note: This function relies on RPC protocol internals. This is generally a
// *bad* idea, but is necessary here due to limitations of the RPC system and
// its asymmetric ingress and egress paths.
//
// TODO(frolv): This should be investigated further and perhaps addressed within
// the RPC system, at the least through a helper function.
size_t TransferService::MaxWriteChunkSize(
    const internal::Context& transfer) const {
  // Start with the user-provided maximum chunk size, which should be the usable
  // payload length on the RPC ingress path after any transport overhead.
  ssize_t max_size = max_chunk_size_bytes_;

  // Subtract the RPC overhead (pw_rpc/internal/packet.proto).
  //
  //   type:       1 byte key, 1 byte value (CLIENT_STREAM)
  //   channel_id: 1 byte key, varint value (calculate from stream)
  //   service_id: 1 byte key, 4 byte value
  //   method_id:  1 byte key, 4 byte value
  //   payload:    1 byte key, varint length (remaining space)
  //   status:     0 bytes (not set in stream packets)
  //
  //   TOTAL: 14 bytes + encoded channel_id size + encoded payload length
  //
  max_size -= 14;
  max_size -= varint::EncodedSize(write_stream_.channel_id());
  max_size -= varint::EncodedSize(max_size);

  // Subtract the transfer service overhead for a client write chunk
  // (pw_transfer/transfer.proto).
  //
  //   transfer_id: 1 byte key, varint value (calculate)
  //   offset:      1 byte key, varint value (calculate)
  //   data:        1 byte key, varint length (remaining space)
  //
  //   TOTAL: 3 + encoded transfer_id + encoded offset + encoded data length
  //
  size_t max_offset_in_window = transfer.offset() + transfer.pending_bytes();
  max_size -= 3;
  max_size -= varint::EncodedSize(transfer.transfer_id());
  max_size -= varint::EncodedSize(max_offset_in_window);
  max_size -= varint::EncodedSize(max_size);

  // A resulting value of zero (or less) renders write transfers unusable, as
  // there is no space to send any payload. This should be considered a
  // programmer error in the transfer service setup.
  PW_CHECK_INT_GT(
      max_size,
      0,
      "Transfer service maximum chunk size is too small to fit a payload. "
      "Increase max_chunk_size_bytes to support write transfers.");

  return max_size;
}

}  // namespace pw::transfer
