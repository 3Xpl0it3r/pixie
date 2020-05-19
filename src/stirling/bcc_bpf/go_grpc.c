// LINT_C_FILE: Do not remove this line. It ensures cpplint treats this as a C file.

#include "src/stirling/bcc_bpf_interface/go_grpc_types.h"

#define HEADER_COUNT 64

BPF_PERF_OUTPUT(go_grpc_header_events);
BPF_PERF_OUTPUT(go_grpc_data_events);

// BPF programs are limited to a 512-byte stack. We store this value per CPU
// and use it as a heap allocated value.
BPF_PERCPU_ARRAY(data_event_buffer_heap, struct go_grpc_data_event_t, 1);
static __inline struct go_grpc_data_event_t* get_data_event() {
  uint32_t kZero = 0;
  return data_event_buffer_heap.lookup(&kZero);
}

// Key: TGID
// Value: Symbol addresses for the binary with that TGID.
BPF_HASH(http2_symaddrs_map, uint32_t, struct conn_symaddrs_t);

// This map is used to help extract HTTP2 headers from the net/http library.
// The tracing process requires multiple probes:
//  - The primary probe collects context and sets this map entry.
//  - Dependent probes trace functions called by the primary function;
//    these read the map to get the context.
//  - The return probe of the primary function deletes the map entry.
//
// Key: encoder instance pointer
// Value: Header attributes (e.g. stream_id, fd)
BPF_HASH(active_write_headers_frame_map, void*, struct header_attr_t);

// From golang source:
// //A HeaderField is a name-value pair. Both the name and value are
// // treated as opaque sequences of octets.
// type HeaderField struct {
//   Name, Value string
//
//   // Sensitive means that this header field should never be
//   // indexed.
//   Sensitive bool
// }
struct HPackHeaderField {
  struct gostring name;
  struct gostring value;
  bool sensitive;
};

// From golang source:
// type FD struct {
//   fdmu fdMutex
//   // fdMutex is 16 bytes.
//   type fdMutex struct {
//     state uint64
//     rsema uint32
//     wsema uint32
//   }
//   Sysfd int
//   ...
// }
struct FD {
  uint64_t fdmu0;
  uint64_t fdmu1;
  int64_t sysfd;
};

// Meaning of flag bits in FrameHeader flags.
// https://github.com/golang/net/blob/master/http2/frame.go
// TODO(oazizi): Use DWARF info to get these values.
const uint8_t kFlagDataEndStream = 0x1;
const uint8_t kFlagHeadersEndStream = 0x1;

#define REQUIRE_SYMADDR(symaddr, retval) \
  if (symaddr == -1) {                   \
    return retval;                       \
  }

//-----------------------------------------------------------------------------
// FD extraction functions
//-----------------------------------------------------------------------------

const int32_t kInvalidFD = -1;

// This function accesses one of the following:
//   conn.conn.conn.fd.pfd.Sysfd
//   conn.conn.fd.pfd.Sysfd
//   conn.fd.pfd.Sysfd
// The right one to use depends on the context (e.g. whether the connection uses TLS or not).
//
// (gdb) x ($sp+8)
// 0xc000069e48:  0x000000c0001560e0
// (gdb) x/2gx (0x000000c0001560e0+112)
// 0xc000156150:  0x0000000000b2b1e0  0x000000c0000caf00
// (gdb) x 0x0000000000b2b1e0
// 0xb2b1e0 <go.itab.*google.golang.org/grpc/internal/transport.bufWriter,io.Writer>:
// 0x00000000009c9400 (gdb) x/2gx (0x000000c0000caf00+40) 0xc0000caf28:  0x0000000000b3bf60
// 0x000000c00000ec20 (gdb) x 0x0000000000b3bf60 0xb3bf60
// <go.itab.*google.golang.org/grpc/credentials/internal.syscallConn,net.Conn>: 0x00000000009f66c0
// (gdb) x/2gx 0x000000c00000ec20
// 0xc00000ec20:  0x0000000000b3bea0  0x000000c000059180
// (gdb) x 0x0000000000b3bea0
// 0xb3bea0 <go.itab.*crypto/tls.Conn,net.Conn>:  0x00000000009f66c0
// (gdb) x/2gx 0x000000c000059180
// 0xc000059180:  0x0000000000b3c020  0x000000c000010048
// (gdb) x 0x0000000000b3c020
// 0xb3c020 <go.itab.*net.TCPConn,net.Conn>:  0x00000000009f66c0
//
//
// Another representation:
//   conn net.Conn
//   type net.Conn interface {
//     ...
//     data  // A pointer to *net.TCPConn, which implements the net.Conn interface.
//     type TCPConn struct {
//       conn  // conn is embedded inside TCPConn, which is defined as follows.
//       type conn struct {
//         fd *netFD
//         type netFD struct {
//           pfd poll.FD
//           type FD struct {
//             ...
//             Sysfd int
//           }
//         }
//       }
//     }
//   }
static __inline int32_t get_fd_from_conn_intf(struct go_interface conn_intf) {
  uint64_t current_pid_tgid = bpf_get_current_pid_tgid();
  uint32_t tgid = current_pid_tgid >> 32;
  struct conn_symaddrs_t* symaddrs = http2_symaddrs_map.lookup(&tgid);
  if (symaddrs == NULL) {
    return kInvalidFD;
  }

  if (conn_intf.type == symaddrs->syscall_conn) {
    const int kSyscallConnConnOffset = 0;
    bpf_probe_read(&conn_intf, sizeof(conn_intf), conn_intf.ptr + kSyscallConnConnOffset);
  }

  if (conn_intf.type == symaddrs->tls_conn) {
    const int kTLSConnConnOffset = 0;
    bpf_probe_read(&conn_intf, sizeof(conn_intf), conn_intf.ptr + kTLSConnConnOffset);
  }

  if (conn_intf.type != symaddrs->tcp_conn) {
    return kInvalidFD;
  }

  void* fd_ptr;
  bpf_probe_read(&fd_ptr, sizeof(fd_ptr), conn_intf.ptr);

  struct FD fd;
  const int kFDOffset = 0;
  bpf_probe_read(&fd, sizeof(fd), fd_ptr + kFDOffset);

  return fd.sysfd;
}

// Returns the file descriptor from a http2.Framer object.
static __inline int32_t get_fd_from_http2_Framer(const void* framer_ptr,
                                                 struct conn_symaddrs_t* symaddrs) {
  REQUIRE_SYMADDR(symaddrs->Framer_w_offset, kInvalidFD);
  REQUIRE_SYMADDR(symaddrs->bufWriter_conn_offset, kInvalidFD);

  struct go_interface io_writer_interface;
  bpf_probe_read(&io_writer_interface, sizeof(io_writer_interface),
                 framer_ptr + symaddrs->Framer_w_offset);

  // At this point, we have the following struct:
  // go.itab.*google.golang.org/grpc/internal/transport.bufWriter,io.Writer

  struct go_interface conn_intf;
  bpf_probe_read(&conn_intf, sizeof(conn_intf),
                 io_writer_interface.ptr + symaddrs->bufWriter_conn_offset);

  return get_fd_from_conn_intf(conn_intf);
}

// Returns the file descriptor from a http.http2Framer object.
static __inline int32_t get_fd_from_http_http2Framer(const void* framer_ptr,
                                                     struct conn_symaddrs_t* symaddrs) {
  REQUIRE_SYMADDR(symaddrs->http2Framer_w_offset, kInvalidFD);
  REQUIRE_SYMADDR(symaddrs->http2bufferedWriter_w_offset, kInvalidFD);

  struct go_interface io_writer_interface;
  bpf_probe_read(&io_writer_interface, sizeof(io_writer_interface),
                 framer_ptr + symaddrs->http2Framer_w_offset);

  // At this point, we have the following struct:
  // go.itab.*net/http.http2bufferedWriter,io.Writer

  // We have to dereference one more time, to get the inner io.Writer:
  struct go_interface inner_io_writer_interface;
  bpf_probe_read(&inner_io_writer_interface, sizeof(inner_io_writer_interface),
                 io_writer_interface.ptr + symaddrs->http2bufferedWriter_w_offset);

  // Now get the struct implementing net.Conn.
  // TODO(oazizi): Convert to using DWARF information.
  const int kIOWriterConnOffset = 0;
  struct go_interface conn_intf;
  bpf_probe_read(&conn_intf, sizeof(conn_intf),
                 inner_io_writer_interface.ptr + kIOWriterConnOffset);

  return get_fd_from_conn_intf(conn_intf);
}

//-----------------------------------------------------------------------------
// HTTP2 Header Tracing Functions
//-----------------------------------------------------------------------------

static __inline void fill_header_field(struct go_grpc_http2_header_event_t* event,
                                       const struct HPackHeaderField* header_ptr) {
  struct HPackHeaderField field;
  bpf_probe_read(&field, sizeof(struct HPackHeaderField), header_ptr);

  // Note that we read one extra byte for name and value.
  // This is to avoid passing a size of 0 to bpf_probe_read(),
  // which causes BPF verifier issues on kernel 4.14.

  event->name.size = BPF_LEN_CAP(field.name.len, HEADER_FIELD_STR_SIZE);
  bpf_probe_read(event->name.msg, event->name.size + 1, field.name.ptr);

  event->value.size = BPF_LEN_CAP(field.value.len, HEADER_FIELD_STR_SIZE);
  bpf_probe_read(event->value.msg, event->value.size + 1, field.value.ptr);
}

// TODO(oazizi): Convert to use symaddrs.
static __inline void submit_headers(struct pt_regs* ctx, enum http2_probe_type_t probe_type,
                                    enum HeaderEventType type, int32_t fd, uint32_t stream_id,
                                    bool end_stream, struct go_ptr_array fields) {
  uint32_t tgid = bpf_get_current_pid_tgid() >> 32;
  struct conn_info_t* conn_info = get_conn_info(tgid, fd);
  if (conn_info == NULL) {
    return;
  }
  conn_info->addr_valid = true;

  struct go_grpc_http2_header_event_t event = {};
  event.attr.type = type;
  event.attr.timestamp_ns = bpf_ktime_get_ns();
  event.attr.conn_id = conn_info->conn_id;
  event.attr.stream_id = stream_id;

  const struct HPackHeaderField* fields_ptr = fields.ptr;
#pragma unroll
  for (unsigned int i = 0; i < HEADER_COUNT && i < fields.len; ++i) {
    fill_header_field(&event, fields_ptr + i);
    go_grpc_header_events.perf_submit(ctx, &event, sizeof(event));
  }

  // If end of stream, send one extra empty header with end-stream flag set.
  if (end_stream) {
    event.name.size = 0;
    event.value.size = 0;
    event.attr.end_stream = true;
    go_grpc_header_events.perf_submit(ctx, &event, sizeof(event));
  }
}

struct go_grpc_framer_t {
  void* writer;
  void* http2_framer;
};

// Probes (*loopyWriter).writeHeader() inside gRPC-go, which writes HTTP2 headers to the server.
//
// Function signature:
//     func (l *loopyWriter) writeHeader(streamID uint32, endStream bool, hf []hpack.HeaderField,
//     onWrite func()) error
//
// Symbol:
//   google.golang.org/grpc/internal/transport.(*loopyWriter).writeHeader
int probe_loopy_writer_write_header(struct pt_regs* ctx) {
  uint32_t tgid = bpf_get_current_pid_tgid() >> 32;
  struct conn_symaddrs_t* symaddrs = http2_symaddrs_map.lookup(&tgid);
  if (symaddrs == NULL) {
    return 0;
  }

  REQUIRE_SYMADDR(symaddrs->loopyWriter_framer_offset, 0);

  // ---------------------------------------------
  // Extract arguments (on stack)
  // ---------------------------------------------

  const void* sp = (const void*)PT_REGS_SP(ctx);

  const int kLoopyWriterParamOffset = 8;
  const void* loopy_writer_ptr = *(const void**)(sp + kLoopyWriterParamOffset);

  const int kStreamIDParamOffset = 16;
  uint32_t stream_id = *(uint32_t*)(sp + kStreamIDParamOffset);

  const int kEndStreamParamOffset = 20;
  bool end_stream = *(bool*)(sp + kEndStreamParamOffset);

  const int kHeaderFieldSliceParamOffset = 24;
  const struct go_ptr_array fields =
      *(const struct go_ptr_array*)(sp + kHeaderFieldSliceParamOffset);

  // ---------------------------------------------
  // Extract members
  // ---------------------------------------------

  const void* framer_ptr = *(const void**)(loopy_writer_ptr + symaddrs->loopyWriter_framer_offset);

  // TODO(oazizi): Stop using mirrored go structs, and use DWARF info instead.
  struct go_grpc_framer_t go_grpc_framer;
  bpf_probe_read(&go_grpc_framer, sizeof(go_grpc_framer), framer_ptr);

  const int32_t fd = get_fd_from_http2_Framer(go_grpc_framer.http2_framer, symaddrs);
  if (fd == kInvalidFD) {
    return 0;
  }

  submit_headers(ctx, k_probe_loopy_writer_write_header, kHeaderEventWrite, fd, stream_id,
                 end_stream, fields);

  return 0;
}

// Shared helper function for:
//   probe_http2_client_operate_headers()
//   probe_http2_server_operate_headers()
// The two probes are similar but the conn_intf location is specific to each struct.
// MetaHeadersFrame_ptr is of type: golang.org/x/net/http2.MetaHeadersFrame
static __inline void probe_http2_operate_headers(struct pt_regs* ctx,
                                                 enum http2_probe_type_t probe_type, int32_t fd,
                                                 const void* MetaHeadersFrame_ptr,
                                                 struct conn_symaddrs_t* symaddrs) {
  REQUIRE_SYMADDR(symaddrs->MetaHeadersFrame_HeadersFrame_offset, /* none */);
  REQUIRE_SYMADDR(symaddrs->MetaHeadersFrame_Fields_offset, /* none */);
  REQUIRE_SYMADDR(symaddrs->HeadersFrame_FrameHeader_offset, /* none */);
  REQUIRE_SYMADDR(symaddrs->FrameHeader_Flags_offset, /* none */);
  REQUIRE_SYMADDR(symaddrs->FrameHeader_StreamID_offset, /* none */);

  // ------------------------------------------------------
  // Extract members of MetaHeadersFrame_ptr (HeadersFrame, Fields)
  // ------------------------------------------------------

  void* HeadersFrame_ptr;
  bpf_probe_read(&HeadersFrame_ptr, sizeof(void*),
                 MetaHeadersFrame_ptr + symaddrs->MetaHeadersFrame_HeadersFrame_offset);

  struct go_ptr_array fields;
  bpf_probe_read(&fields, sizeof(struct go_ptr_array),
                 MetaHeadersFrame_ptr + symaddrs->MetaHeadersFrame_Fields_offset);

  // ------------------------------------------------------
  // Extract members of HeadersFrame_ptr (HeadersFrame)
  // ------------------------------------------------------

  void* FrameHeader_ptr = HeadersFrame_ptr + symaddrs->HeadersFrame_FrameHeader_offset;

  // ------------------------------------------------------
  // Extract members of FrameHeader_ptr (stream_id, end_stream)
  // ------------------------------------------------------

  uint8_t flags;
  bpf_probe_read(&flags, sizeof(uint8_t), FrameHeader_ptr + symaddrs->FrameHeader_Flags_offset);
  const bool end_stream = flags & kFlagHeadersEndStream;

  uint32_t stream_id;
  bpf_probe_read(&stream_id, sizeof(uint32_t),
                 FrameHeader_ptr + symaddrs->FrameHeader_StreamID_offset);

  // ------------------------------------------------------
  // Submit
  // ------------------------------------------------------

  // TODO(yzhao): We saw some arbitrary large slices received by operateHeaders(), it's not clear
  // what conditions result into them.
  if (fields.len > 100 || fields.len <= 0 || fields.cap <= 0) {
    return;
  }

  submit_headers(ctx, probe_type, kHeaderEventRead, fd, stream_id, end_stream, fields);
}

// Probe for the golang.org/x/net/http2 library's header reader (client-side).
//
// Probes (*http2Client).operateHeaders(*http2.MetaHeadersFrame) inside gRPC-go, which processes
// HTTP2 headers of the received responses.
//
// Function signature:
//   func (t *http2Client) operateHeaders(frame *http2.MetaHeadersFrame)
//
// Symbol:
//   google.golang.org/grpc/internal/transport.(*http2Client).operateHeaders
int probe_http2_client_operate_headers(struct pt_regs* ctx) {
  uint32_t tgid = bpf_get_current_pid_tgid() >> 32;
  struct conn_symaddrs_t* symaddrs = http2_symaddrs_map.lookup(&tgid);
  if (symaddrs == NULL) {
    return 0;
  }

  REQUIRE_SYMADDR(symaddrs->http2Client_conn_offset, 0);

  // ---------------------------------------------
  // Extract arguments (on stack)
  // ---------------------------------------------

  const void* sp = (const void*)PT_REGS_SP(ctx);

  const int kHTTP2ClientParamOffset = 8;
  const void* http2_client_ptr = *(const void**)(sp + kHTTP2ClientParamOffset);

  const int kFrameParamOffset = 16;
  const void* frame_ptr = *(const void**)(sp + kFrameParamOffset);

  // ---------------------------------------------
  // Extract arguments (on stack)
  // ---------------------------------------------

  struct go_interface conn_intf;
  bpf_probe_read(&conn_intf, sizeof(conn_intf),
                 http2_client_ptr + symaddrs->http2Client_conn_offset);

  const int32_t fd = get_fd_from_conn_intf(conn_intf);
  if (fd == kInvalidFD) {
    return 0;
  }

  probe_http2_operate_headers(ctx, k_probe_http2_client_operate_headers, fd, frame_ptr, symaddrs);

  return 0;
}

// Probe for the golang.org/x/net/http2 library's header reader (server-side).
//
// Function signature:
//   func (t *http2Server) operateHeaders(frame *http2.MetaHeadersFrame, handle func(*Stream),
//                                        traceCtx func(context.Context, string) context.Context
//                                        (fatal bool)
// Symbol:
//   google.golang.org/grpc/internal/transport.(*http2Server).operateHeaders
int probe_http2_server_operate_headers(struct pt_regs* ctx) {
  uint32_t tgid = bpf_get_current_pid_tgid() >> 32;
  struct conn_symaddrs_t* symaddrs = http2_symaddrs_map.lookup(&tgid);
  if (symaddrs == NULL) {
    return 0;
  }

  REQUIRE_SYMADDR(symaddrs->http2Server_conn_offset, 0);

  // ---------------------------------------------
  // Extract arguments (on stack)
  // ---------------------------------------------

  const void* sp = (const void*)PT_REGS_SP(ctx);

  const int kHTTP2ServerParamOffset = 8;
  const void* http2_server_ptr = *(const void**)(sp + kHTTP2ServerParamOffset);

  const int kFrameParamOffset = 16;
  const void* frame_ptr = *(const void**)(sp + kFrameParamOffset);

  // ---------------------------------------------
  // Extract members
  // ---------------------------------------------

  struct go_interface conn_intf;
  bpf_probe_read(&conn_intf, sizeof(conn_intf),
                 http2_server_ptr + symaddrs->http2Server_conn_offset);

  const int32_t fd = get_fd_from_conn_intf(conn_intf);
  if (fd == kInvalidFD) {
    return 0;
  }

  probe_http2_operate_headers(ctx, k_probe_http2_server_operate_headers, fd, frame_ptr, symaddrs);

  return 0;
}

// Probe for the net/http library's header reader.
//
// Function signature:
//   func (sc *http2serverConn) processHeaders(f *http2MetaHeadersFrame) error
//
// Symbol:
//   net/http.(*http2serverConn).processHeaders
//
// Verified to be stable from go1.?? to t go.1.13.
int probe_http_http2serverConn_processHeaders(struct pt_regs* ctx) {
  uint32_t tgid = bpf_get_current_pid_tgid() >> 32;
  struct conn_symaddrs_t* symaddrs = http2_symaddrs_map.lookup(&tgid);
  if (symaddrs == NULL) {
    return 0;
  }

  REQUIRE_SYMADDR(symaddrs->http2MetaHeadersFrame_http2HeadersFrame_offset, 0);
  REQUIRE_SYMADDR(symaddrs->http2MetaHeadersFrame_Fields_offset, 0);
  REQUIRE_SYMADDR(symaddrs->http2HeadersFrame_http2FrameHeader_offset, 0);
  REQUIRE_SYMADDR(symaddrs->http2FrameHeader_Flags_offset, 0);
  REQUIRE_SYMADDR(symaddrs->http2FrameHeader_StreamID_offset, 0);
  REQUIRE_SYMADDR(symaddrs->http2serverConn_conn_offset, 0);

  // ---------------------------------------------
  // Extract arguments (on stack)
  // ---------------------------------------------

  const void* sp = (const void*)PT_REGS_SP(ctx);

  // Receiver is (*http2serverConn).
  const int kReceiverOffset = 8;
  void* http2serverConn_ptr;
  bpf_probe_read(&http2serverConn_ptr, sizeof(void*), sp + kReceiverOffset);

  // Param 1 is (*http2MetaHeadersFrame).
  const int kParam1Offset = 16;
  void* http2MetaHeadersFrame_ptr;
  bpf_probe_read(&http2MetaHeadersFrame_ptr, sizeof(void*), sp + kParam1Offset);

  // ------------------------------------------------------
  // Extract members of http2MetaHeadersFrame_ptr (headers)
  // ------------------------------------------------------

  struct go_ptr_array fields;
  bpf_probe_read(&fields, sizeof(struct go_ptr_array),
                 http2MetaHeadersFrame_ptr + symaddrs->http2MetaHeadersFrame_Fields_offset);

  void* http2HeadersFrame_ptr;
  bpf_probe_read(
      &http2HeadersFrame_ptr, sizeof(void*),
      http2MetaHeadersFrame_ptr + symaddrs->http2MetaHeadersFrame_http2HeadersFrame_offset);

  // ------------------------------------------------------
  // Extract members of http2HeadersFrame_ptr (stream_id, end_stream)
  // ------------------------------------------------------

  void* http2FrameHeader_ptr =
      http2HeadersFrame_ptr + symaddrs->http2HeadersFrame_http2FrameHeader_offset;

  uint8_t flags;
  bpf_probe_read(&flags, sizeof(uint8_t),
                 http2FrameHeader_ptr + symaddrs->http2FrameHeader_Flags_offset);
  const bool end_stream = flags & kFlagHeadersEndStream;

  uint32_t stream_id;
  bpf_probe_read(&stream_id, sizeof(uint32_t),
                 http2FrameHeader_ptr + symaddrs->http2FrameHeader_StreamID_offset);

  // ------------------------------------------------------
  // Extract members of http2serverConn_ptr (fd)
  // ------------------------------------------------------

  struct go_interface conn_intf;
  bpf_probe_read(&conn_intf, sizeof(conn_intf),
                 http2serverConn_ptr + symaddrs->http2serverConn_conn_offset);

  const int32_t fd = get_fd_from_conn_intf(conn_intf);
  if (fd == kInvalidFD) {
    return 0;
  }

  // ------------------------------------------------------
  // Wrap-ups
  // ------------------------------------------------------

  submit_headers(ctx, k_probe_http_http2serverConn_processHeaders, kHeaderEventRead, fd, stream_id,
                 end_stream, fields);

  return 0;
}

// TODO(oazizi): Convert to use symaddrs.
static __inline void submit_header(struct pt_regs* ctx, enum http2_probe_type_t probe_type,
                                   enum HeaderEventType type, void* encoder_ptr,
                                   struct HPackHeaderField* header_field_ptr) {
  struct header_attr_t* attr = active_write_headers_frame_map.lookup(&encoder_ptr);
  if (attr == NULL) {
    return;
  }

  struct HPackHeaderField header_field;
  bpf_probe_read(&header_field, sizeof(header_field), header_field_ptr);

  struct go_grpc_http2_header_event_t event = {};
  event.attr.probe_type = probe_type;
  event.attr.type = type;
  event.attr.timestamp_ns = bpf_ktime_get_ns();
  event.attr.conn_id = attr->conn_id;
  event.attr.stream_id = attr->stream_id;

  fill_header_field(&event, &header_field);
  go_grpc_header_events.perf_submit(ctx, &event, sizeof(event));
}

// Probe for the hpack's header encoder.
//
// Function signature:
//   func (e *Encoder) WriteField(f HeaderField) error
//
// Symbol:
//   golang.org/x/net/http2/hpack.(*Encoder).WriteField
//
// Verified to be stable from at least go1.6 to t go.1.13.
int probe_hpack_header_encoder(struct pt_regs* ctx) {
  const void* sp = (const void*)ctx->sp;
  if (sp == NULL) {
    return 0;
  }

  // Receiver is (*Encoder).
  const int kReceiverOffset = 8;
  void* encoder_ptr;
  bpf_probe_read(&encoder_ptr, sizeof(void*), sp + kReceiverOffset);

  // Param 1 is (HeaderField).
  const int kParam1Offset = 16;
  struct HPackHeaderField header_field;
  bpf_probe_read(&header_field, sizeof(struct HPackHeaderField), sp + kParam1Offset);

  submit_header(ctx, k_probe_hpack_header_encoder, kHeaderEventWrite, encoder_ptr, &header_field);

  return 0;
}

// Probe for the net/http library's header writer.
//
// Function signature:
//   func (w *http2writeResHeaders) writeFrame(ctx http2writeContext) error {
//
// Symbol:
//   net/http.(*http2writeResHeaders).writeFrame
//
// Verified to be stable from go1.?? to t go.1.13.
int probe_http_http2writeResHeaders_write_frame(struct pt_regs* ctx) {
  uint32_t tgid = bpf_get_current_pid_tgid() >> 32;
  struct conn_symaddrs_t* symaddrs = http2_symaddrs_map.lookup(&tgid);
  if (symaddrs == NULL) {
    return 0;
  }

  REQUIRE_SYMADDR(symaddrs->http2serverConn_hpackEncoder_offset, 0);
  REQUIRE_SYMADDR(symaddrs->http2serverConn_conn_offset, 0);
  REQUIRE_SYMADDR(symaddrs->http2writeResHeaders_streamID_offset, 0);
  REQUIRE_SYMADDR(symaddrs->http2writeResHeaders_endStream_offset, 0);

  // ---------------------------------------------
  // Extract arguments (on stack)
  // ---------------------------------------------

  const void* sp = (const void*)PT_REGS_SP(ctx);

  // Receiver is (*http2writeResHeaders).
  const int kReceiverOffset = 8;
  void* http2writeResHeaders_ptr;
  bpf_probe_read(&http2writeResHeaders_ptr, sizeof(void*), sp + kReceiverOffset);

  // Param 1 is (http2writeContext).
  const int kParam1Offset = 16;
  struct go_interface http2writeContext;
  bpf_probe_read(&http2writeContext, sizeof(struct go_interface), sp + kParam1Offset);

  void* http2serverConn_ptr = http2writeContext.ptr;

  // ------------------------------------------------------
  // Extract members of http2writeResHeaders_ptr (stream_id, end_stream)
  // ------------------------------------------------------

  uint32_t stream_id;
  bpf_probe_read(&stream_id, sizeof(uint32_t),
                 http2writeResHeaders_ptr + symaddrs->http2writeResHeaders_streamID_offset);

  bool end_stream;
  bpf_probe_read(&end_stream, sizeof(bool),
                 http2writeResHeaders_ptr + symaddrs->http2writeResHeaders_endStream_offset);

  // ------------------------------------------------------
  // Extract members of http2serverConn_ptr (encoder, fd)
  // ------------------------------------------------------

  void* henc_addr;
  bpf_probe_read(&henc_addr, sizeof(void*),
                 http2serverConn_ptr + symaddrs->http2serverConn_hpackEncoder_offset);

  struct go_interface conn_intf;
  bpf_probe_read(&conn_intf, sizeof(conn_intf),
                 http2serverConn_ptr + symaddrs->http2serverConn_conn_offset);

  const int32_t fd = get_fd_from_conn_intf(conn_intf);
  if (fd == kInvalidFD) {
    return 0;
  }

  // ------------------------------------------------------
  // Prepare to submit headers to perf buffer
  // ------------------------------------------------------

  struct conn_info_t* conn_info = get_conn_info(tgid, fd);
  if (conn_info == NULL) {
    return 0;
  }
  conn_info->addr_valid = true;

  struct header_attr_t attr = {};
  attr.conn_id = conn_info->conn_id;
  attr.stream_id = stream_id;

  // We don't have the header values yet, and they are not easy to get from this probe,
  // so we just stash the information collected so far.
  // A separate probe, on the hpack encoder monitors the headers being encoded,
  // and joins that information with the stashed information collected here.
  // The key is the encoder instance.
  active_write_headers_frame_map.update(&henc_addr, &attr);

  // TODO(oazizi): Content beyond this point needs to move to return probe of the same function.

  if (end_stream) {
    struct go_grpc_http2_header_event_t event = {};
    event.attr.probe_type = k_probe_http_http2writeResHeaders_write_frame;
    event.attr.type = kHeaderEventWrite;
    event.attr.timestamp_ns = bpf_ktime_get_ns();
    event.attr.conn_id = conn_info->conn_id;
    event.attr.stream_id = stream_id;
    event.name.size = 0;
    event.value.size = 0;
    event.attr.end_stream = true;
    go_grpc_header_events.perf_submit(ctx, &event, sizeof(event));
  }

  // TODO(oazizi): We are leaking BPF map entries until this line is activated,
  // which can only happen once we have return probes enabled.
  // active_write_headers_frame_map.update(&henc_addr, &attr);

  return 0;
}

//-----------------------------------------------------------------------------
// HTTP2 Data Tracing Functions
//-----------------------------------------------------------------------------

static __inline void submit_data(struct pt_regs* ctx, uint32_t tgid, int32_t fd,
                                 enum DataFrameEventType type, uint32_t stream_id, bool end_stream,
                                 struct go_byte_array data) {
  struct conn_info_t* conn_info = get_conn_info(tgid, fd);
  if (conn_info == NULL) {
    return;
  }
  conn_info->addr_valid = true;

  struct go_grpc_data_event_t* info = get_data_event();
  if (info == NULL) {
    return;
  }

  info->attr.conn_id = conn_info->conn_id;
  info->attr.timestamp_ns = bpf_ktime_get_ns();
  info->attr.type = type;
  info->attr.stream_id = stream_id;
  info->attr.end_stream = end_stream;
  uint32_t data_len = BPF_LEN_CAP(data.len, MAX_DATA_SIZE);
  info->attr.data_len = data_len;
  bpf_probe_read(info->data, data_len + 1, data.ptr);

  go_grpc_data_events.perf_submit(ctx, info, sizeof(info->attr) + data_len);
}

// Probes golang.org/x/net/http2.Framer for payload.
//
// As a proxy for the return probe on ReadFrame(), we currently probe checkFrameOrder,
// since return probes don't work for Go.
//
// Function signature:
//   func (fr *Framer) checkFrameOrder(f Frame) error
//
// Symbol:
//   golang.org/x/net/http2.(*Framer).checkFrameOrder
//
// Verified to be stable from at least go1.6 to t go.1.13.
int probe_http2_framer_check_frame_order(struct pt_regs* ctx) {
  uint32_t tgid = bpf_get_current_pid_tgid() >> 32;
  struct conn_symaddrs_t* symaddrs = http2_symaddrs_map.lookup(&tgid);
  if (symaddrs == NULL) {
    return 0;
  }

  REQUIRE_SYMADDR(symaddrs->FrameHeader_Type_offset, 0);
  REQUIRE_SYMADDR(symaddrs->FrameHeader_Flags_offset, 0);
  REQUIRE_SYMADDR(symaddrs->FrameHeader_StreamID_offset, 0);
  REQUIRE_SYMADDR(symaddrs->DataFrame_data_offset, 0);

  // ---------------------------------------------
  // Extract arguments (on stack)
  // ---------------------------------------------

  const char* sp = (const char*)ctx->sp;

  // Param 0 (receiver) is (*Framer).
  const int kParam0Offset = 8;
  void* framer_ptr;
  bpf_probe_read(&framer_ptr, sizeof(void*), sp + kParam0Offset);

  // Param 1 is (Frame)
  const int kParam1Offset = 16;
  struct go_interface frame_interface;
  bpf_probe_read(&frame_interface, sizeof(struct go_interface), sp + kParam1Offset);

  // ------------------------------------------------------
  // Extract members of Framer (fd)
  // ------------------------------------------------------

  int32_t fd = get_fd_from_http2_Framer(framer_ptr, symaddrs);
  if (fd == kInvalidFD) {
    return 0;
  }

  // ------------------------------------------------------
  // Extract members of FrameHeader (type, flags, stream_id)
  // ------------------------------------------------------

  // All Frame types start with a frame header, so this is safe.
  // TODO(oazizi): Is there a more robust way based on DWARF info.
  // This would be required for dynamic tracing anyways.
  void* frame_header_ptr = frame_interface.ptr;

  uint8_t frame_type;
  bpf_probe_read(&frame_type, sizeof(uint8_t),
                 frame_header_ptr + symaddrs->FrameHeader_Type_offset);

  uint8_t flags;
  bpf_probe_read(&flags, sizeof(uint8_t), frame_header_ptr + symaddrs->FrameHeader_Flags_offset);
  const bool end_stream = flags & kFlagDataEndStream;

  uint32_t stream_id;
  bpf_probe_read(&stream_id, sizeof(uint32_t),
                 frame_header_ptr + symaddrs->FrameHeader_StreamID_offset);

  // Consider only data frames (0).
  if (frame_type != 0) {
    return 0;
  }

  // ------------------------------------------------------
  // Extract members of DataFrame (data)
  // ------------------------------------------------------

  // Reinterpret as data frame.
  void* data_frame_ptr = frame_interface.ptr;

  struct go_byte_array data;
  bpf_probe_read(&data, sizeof(struct go_byte_array),
                 data_frame_ptr + symaddrs->DataFrame_data_offset);

  // ------------------------------------------------------
  // Submit
  // ------------------------------------------------------

  submit_data(ctx, tgid, fd, kDataFrameEventRead, stream_id, end_stream, data);
  return 0;
}

// Probes net/http.http2Framer for HTTP2 payload.
//
// As a proxy for the return probe on ReadFrame(), we currently probe checkFrameOrder,
// since return probes don't work for Go.
//
// Function signature:
//   func (fr *http2Framer) checkFrameOrder(f http2Frame) error
//
// Symbol:
//   net/http.(*http2Framer).checkFrameOrder
//
// Verified to be stable from at least go1.?? to go.1.13.
int probe_http_http2framer_check_frame_order(struct pt_regs* ctx) {
  uint32_t tgid = bpf_get_current_pid_tgid() >> 32;
  struct conn_symaddrs_t* symaddrs = http2_symaddrs_map.lookup(&tgid);
  if (symaddrs == NULL) {
    return 0;
  }

  REQUIRE_SYMADDR(symaddrs->http2FrameHeader_Type_offset, 0);
  REQUIRE_SYMADDR(symaddrs->http2FrameHeader_Flags_offset, 0);
  REQUIRE_SYMADDR(symaddrs->http2FrameHeader_StreamID_offset, 0);
  REQUIRE_SYMADDR(symaddrs->http2DataFrame_data_offset, 0);

  // ---------------------------------------------
  // Extract arguments (on stack)
  // ---------------------------------------------

  const char* sp = (const char*)ctx->sp;

  // Param 0 (receiver) is (*http2Framer).
  const int kParam0Offset = 8;
  void* framer_ptr;
  bpf_probe_read(&framer_ptr, sizeof(void*), sp + kParam0Offset);

  // Param 1 is (http2Frame)
  const int kParam1Offset = 16;
  struct go_interface frame_interface;
  bpf_probe_read(&frame_interface, sizeof(struct go_interface), sp + kParam1Offset);

  // ------------------------------------------------------
  // Extract members of Framer (fd)
  // ------------------------------------------------------

  int32_t fd = get_fd_from_http_http2Framer(framer_ptr, symaddrs);
  if (fd == kInvalidFD) {
    return 0;
  }

  // ------------------------------------------------------
  // Extract members of http2FrameHeader (type, flags, stream_id)
  // ------------------------------------------------------

  // All Frame types start with a frame header, so this is safe.
  // TODO(oazizi): Is there a more robust way based on DWARF info.
  // This would be required for dynamic tracing anyways.
  void* frame_header_ptr = frame_interface.ptr;

  uint8_t frame_type;
  bpf_probe_read(&frame_type, sizeof(uint8_t),
                 frame_header_ptr + symaddrs->http2FrameHeader_Type_offset);

  uint8_t flags;
  bpf_probe_read(&flags, sizeof(uint8_t),
                 frame_header_ptr + symaddrs->http2FrameHeader_Flags_offset);
  const bool end_stream = flags & kFlagDataEndStream;

  uint32_t stream_id;
  bpf_probe_read(&stream_id, sizeof(uint32_t),
                 frame_header_ptr + symaddrs->http2FrameHeader_StreamID_offset);

  // Consider only data frames (0).
  if (frame_type != 0) {
    return 0;
  }

  // ------------------------------------------------------
  // Extract members of DataFrame (data)
  // ------------------------------------------------------

  // Reinterpret as data frame.
  void* data_frame_ptr = frame_interface.ptr;

  struct go_byte_array data;
  bpf_probe_read(&data, sizeof(struct go_byte_array),
                 data_frame_ptr + symaddrs->http2DataFrame_data_offset);

  // ------------------------------------------------------
  // Submit
  // ------------------------------------------------------

  submit_data(ctx, tgid, fd, kDataFrameEventRead, stream_id, end_stream, data);
  return 0;
}

// Probe for the golang.org/x/net/http2 library's frame writer.
//
// Function signature:
//   func (f *Framer) WriteDataPadded(streamID uint32, endStream bool, data, pad []byte) error
//
// Symbol:
//   golang.org/x/net/http2.(*Framer).WriteDataPadded
//
// Verified to be stable from go1.7 to t go.1.13.
int probe_http2_framer_write_data(struct pt_regs* ctx) {
  uint32_t tgid = bpf_get_current_pid_tgid() >> 32;
  struct conn_symaddrs_t* symaddrs = http2_symaddrs_map.lookup(&tgid);
  if (symaddrs == NULL) {
    return 0;
  }

  // ---------------------------------------------
  // Extract arguments (on stack)
  // ---------------------------------------------

  const char* sp = (const char*)ctx->sp;

  // Param 0 (receiver) is (fr *Framer).
  const int kParam0Offset = 8;
  void* framer_ptr;
  bpf_probe_read(&framer_ptr, sizeof(void*), sp + kParam0Offset);

  // Param 1 is (streamID uint32).
  const int kParam1Offset = 16;
  uint32_t stream_id;
  bpf_probe_read(&stream_id, sizeof(uint32_t), sp + kParam1Offset);

  // Param 2 is (endStream bool).
  const int kParam2Offset = 20;
  bool end_stream;
  bpf_probe_read(&end_stream, sizeof(bool), sp + kParam2Offset);

  // Param 3 is (data []byte).
  const int kParam3Offset = 24;
  struct go_byte_array data;
  bpf_probe_read(&data, sizeof(struct go_byte_array), sp + kParam3Offset);

  // ------------------------------------------------------
  // Extract members of Framer (fd)
  // ------------------------------------------------------

  int32_t fd = get_fd_from_http2_Framer(framer_ptr, symaddrs);
  if (fd == kInvalidFD) {
    return 0;
  }

  // ---------------------------------------------
  // Submit
  // ---------------------------------------------

  submit_data(ctx, tgid, fd, kDataFrameEventWrite, stream_id, end_stream, data);

  return 0;
}

// Probe for the net/http library's frame writer.
//
// Function signature:
//   func (f *http2Framer) WriteDataPadded(streamID uint32, endStream bool, data, pad []byte) error
//
// Symbol:
//   net/http.(*http2Framer).WriteDataPadded
//
// Verified to be stable from go1.?? to t go.1.13.
int probe_http_http2framer_write_data(struct pt_regs* ctx) {
  uint32_t tgid = bpf_get_current_pid_tgid() >> 32;
  struct conn_symaddrs_t* symaddrs = http2_symaddrs_map.lookup(&tgid);
  if (symaddrs == NULL) {
    return 0;
  }

  // ---------------------------------------------
  // Extract arguments (on stack)
  // ---------------------------------------------

  const char* sp = (const char*)ctx->sp;

  // Param 0 (receiver) is (fr *Framer).
  const int kParam0Offset = 8;
  void* framer_ptr;
  bpf_probe_read(&framer_ptr, sizeof(void*), sp + kParam0Offset);

  // Param 1 is (streamID uint32).
  const int kParam1Offset = 16;
  uint32_t stream_id;
  bpf_probe_read(&stream_id, sizeof(uint32_t), sp + kParam1Offset);

  // Param 2 is (endStream bool).
  const int kParam2Offset = 20;
  bool end_stream;
  bpf_probe_read(&end_stream, sizeof(bool), sp + kParam2Offset);

  // Param 3 is (data []byte).
  const int kParam3Offset = 24;
  struct go_byte_array data;
  bpf_probe_read(&data, sizeof(struct go_byte_array), sp + kParam3Offset);

  // ------------------------------------------------------
  // Extract members of Framer (fd)
  // ------------------------------------------------------

  int32_t fd = get_fd_from_http_http2Framer(framer_ptr, symaddrs);
  if (fd == kInvalidFD) {
    return 0;
  }

  // ---------------------------------------------
  // Submit
  // ---------------------------------------------

  submit_data(ctx, tgid, fd, kDataFrameEventWrite, stream_id, end_stream, data);

  return 0;
}
