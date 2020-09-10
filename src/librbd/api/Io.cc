// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/api/Io.h"
#include "common/dout.h"
#include "common/errno.h"
#include "common/Cond.h"
#include "common/EventTrace.h"
#include "librbd/ImageCtx.h"
#include "librbd/internal.h"
#include "librbd/Utils.h"
#include "librbd/io/AioCompletion.h"
#include "librbd/io/ImageDispatchSpec.h"
#include "librbd/io/Types.h"

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::api::Io " << __func__ << ": "

namespace librbd {
namespace api {

namespace {

template <typename I>
bool is_valid_io(I& image_ctx, io::AioCompletion* aio_comp) {
  auto cct = image_ctx.cct;

  if (!image_ctx.data_ctx.is_valid()) {
    lderr(cct) << "missing data pool" << dendl;

    aio_comp->fail(-ENODEV);
    return false;
  }

  return true;
}

} // anonymous namespace

template <typename I>
ssize_t Io<I>::read(
    I &image_ctx, uint64_t off, uint64_t len, io::ReadResult &&read_result,
    int op_flags) {
  auto cct = image_ctx.cct;

  ldout(cct, 20) << "ictx=" << &image_ctx << ", off=" << off << ", "
                 << "len = " << len << dendl;

  C_SaferCond ctx;
  auto aio_comp = io::AioCompletion::create(&ctx);
  aio_read(image_ctx, aio_comp, off, len, std::move(read_result), op_flags,
           false);
  return ctx.wait();
}

template <typename I>
ssize_t Io<I>::write(
    I &image_ctx, uint64_t off, uint64_t len, bufferlist &&bl, int op_flags) {
  auto cct = image_ctx.cct;
  ldout(cct, 20) << "ictx=" << &image_ctx << ", off=" << off << ", "
                 << "len = " << len << dendl;

  image_ctx.image_lock.lock_shared();
  int r = clip_io(util::get_image_ctx(&image_ctx), off, &len);
  image_ctx.image_lock.unlock_shared();
  if (r < 0) {
    lderr(cct) << "invalid IO request: " << cpp_strerror(r) << dendl;
    return r;
  }

  C_SaferCond ctx;
  auto aio_comp = io::AioCompletion::create(&ctx);
  aio_write(image_ctx, aio_comp, off, len, std::move(bl), op_flags, false);

  r = ctx.wait();
  if (r < 0) {
    return r;
  }
  return len;
}

template <typename I>
ssize_t Io<I>::discard(
    I &image_ctx, uint64_t off, uint64_t len,
    uint32_t discard_granularity_bytes) {
  auto cct = image_ctx.cct;
  ldout(cct, 20) << "ictx=" << &image_ctx << ", off=" << off << ", "
                 << "len = " << len << dendl;

  image_ctx.image_lock.lock_shared();
  int r = clip_io(util::get_image_ctx(&image_ctx), off, &len);
  image_ctx.image_lock.unlock_shared();
  if (r < 0) {
    lderr(cct) << "invalid IO request: " << cpp_strerror(r) << dendl;
    return r;
  }

  C_SaferCond ctx;
  auto aio_comp = io::AioCompletion::create(&ctx);
  aio_discard(image_ctx, aio_comp, off, len, discard_granularity_bytes, false);

  r = ctx.wait();
  if (r < 0) {
    return r;
  }
  return len;
}

template <typename I>
ssize_t Io<I>::write_same(
    I &image_ctx, uint64_t off, uint64_t len, bufferlist &&bl, int op_flags) {
  auto cct = image_ctx.cct;
  ldout(cct, 20) << "ictx=" << &image_ctx << ", off=" << off << ", "
                 << "len = " << len << ", data_len " << bl.length() << dendl;

  image_ctx.image_lock.lock_shared();
  int r = clip_io(util::get_image_ctx(&image_ctx), off, &len);
  image_ctx.image_lock.unlock_shared();
  if (r < 0) {
    lderr(cct) << "invalid IO request: " << cpp_strerror(r) << dendl;
    return r;
  }

  C_SaferCond ctx;
  auto aio_comp = io::AioCompletion::create(&ctx);
  aio_write_same(image_ctx, aio_comp, off, len, std::move(bl), op_flags, false);

  r = ctx.wait();
  if (r < 0) {
    return r;
  }
  return len;
}

template <typename I>
ssize_t Io<I>::compare_and_write(
    I &image_ctx, uint64_t off, uint64_t len, bufferlist &&cmp_bl,
    bufferlist &&bl, uint64_t *mismatch_off, int op_flags) {
  auto cct = image_ctx.cct;
  ldout(cct, 20) << "compare_and_write ictx=" << &image_ctx << ", off="
                 << off << ", " << "len = " << len << dendl;

  image_ctx.image_lock.lock_shared();
  int r = clip_io(util::get_image_ctx(&image_ctx), off, &len);
  image_ctx.image_lock.unlock_shared();
  if (r < 0) {
    lderr(cct) << "invalid IO request: " << cpp_strerror(r) << dendl;
    return r;
  }

  C_SaferCond ctx;
  auto aio_comp = io::AioCompletion::create(&ctx);
  aio_compare_and_write(image_ctx, aio_comp, off, len, std::move(cmp_bl),
                        std::move(bl), mismatch_off, op_flags, false);

  r = ctx.wait();
  if (r < 0) {
    return r;
  }
  return len;
}

template <typename I>
int Io<I>::flush(I &image_ctx) {
  auto cct = image_ctx.cct;
  ldout(cct, 20) << "ictx=" << &image_ctx << dendl;

  C_SaferCond ctx;
  auto aio_comp = io::AioCompletion::create(&ctx);
  aio_flush(image_ctx, aio_comp, false);

  int r = ctx.wait();
  if (r < 0) {
    return r;
  }

  return 0;
}

template <typename I>
void Io<I>::aio_read(I &image_ctx, io::AioCompletion *aio_comp, uint64_t off,
                     uint64_t len, io::ReadResult &&read_result, int op_flags,
                     bool native_async) {
  auto cct = image_ctx.cct;
  FUNCTRACE(cct);
  ZTracer::Trace trace;
  if (image_ctx.blkin_trace_all) {
    trace.init("io: read", &image_ctx.trace_endpoint);
    trace.event("init");
  }

  aio_comp->init_time(util::get_image_ctx(&image_ctx), io::AIO_TYPE_READ);
  ldout(cct, 20) << "ictx=" << &image_ctx << ", "
                 << "completion=" << aio_comp << ", off=" << off << ", "
                 << "len=" << len << ", " << "flags=" << op_flags << dendl;

  if (native_async && image_ctx.event_socket.is_valid()) {
    aio_comp->set_event_notify(true);
  }

  if (!is_valid_io(image_ctx, aio_comp)) {
    return;
  }

  auto req = io::ImageDispatchSpec<I>::create_read(
    image_ctx, io::IMAGE_DISPATCH_LAYER_API_START, aio_comp, {{off, len}},
    std::move(read_result), op_flags, trace);
  req->send();
}

template <typename I>
void Io<I>::aio_write(I &image_ctx, io::AioCompletion *aio_comp, uint64_t off,
                      uint64_t len, bufferlist &&bl, int op_flags,
                      bool native_async) {
  auto cct = image_ctx.cct;
  FUNCTRACE(cct);
  ZTracer::Trace trace;
  if (image_ctx.blkin_trace_all) {
    trace.init("io: write", &image_ctx.trace_endpoint);
    trace.event("init");
  }

  aio_comp->init_time(util::get_image_ctx(&image_ctx), io::AIO_TYPE_WRITE);
  ldout(cct, 20) << "ictx=" << &image_ctx << ", "
                 << "completion=" << aio_comp << ", off=" << off << ", "
                 << "len=" << len << ", flags=" << op_flags << dendl;

  if (native_async && image_ctx.event_socket.is_valid()) {
    aio_comp->set_event_notify(true);
  }

  if (!is_valid_io(image_ctx, aio_comp)) {
    return;
  }

  auto req = io::ImageDispatchSpec<I>::create_write(
    image_ctx, io::IMAGE_DISPATCH_LAYER_API_START, aio_comp, {{off, len}},
    std::move(bl), op_flags, trace, 0);
  req->send();
}

template <typename I>
void Io<I>::aio_discard(I &image_ctx, io::AioCompletion *aio_comp, uint64_t off,
                        uint64_t len, uint32_t discard_granularity_bytes,
                        bool native_async) {
  auto cct = image_ctx.cct;
  FUNCTRACE(cct);
  ZTracer::Trace trace;
  if (image_ctx.blkin_trace_all) {
    trace.init("io: discard", &image_ctx.trace_endpoint);
    trace.event("init");
  }

  aio_comp->init_time(util::get_image_ctx(&image_ctx), io::AIO_TYPE_DISCARD);
  ldout(cct, 20) << "ictx=" << &image_ctx << ", "
                 << "completion=" << aio_comp << ", off=" << off << ", "
                 << "len=" << len << dendl;

  if (native_async && image_ctx.event_socket.is_valid()) {
    aio_comp->set_event_notify(true);
  }

  if (!is_valid_io(image_ctx, aio_comp)) {
    return;
  }

  auto req = io::ImageDispatchSpec<I>::create_discard(
    image_ctx, io::IMAGE_DISPATCH_LAYER_API_START, aio_comp, off, len,
    discard_granularity_bytes, trace, 0);
  req->send();
}

template <typename I>
void Io<I>::aio_write_same(I &image_ctx, io::AioCompletion *aio_comp,
                           uint64_t off, uint64_t len, bufferlist &&bl,
                           int op_flags, bool native_async) {
  auto cct = image_ctx.cct;
  FUNCTRACE(cct);
  ZTracer::Trace trace;
  if (image_ctx.blkin_trace_all) {
    trace.init("io: writesame", &image_ctx.trace_endpoint);
    trace.event("init");
  }

  aio_comp->init_time(util::get_image_ctx(&image_ctx), io::AIO_TYPE_WRITESAME);
  ldout(cct, 20) << "ictx=" << &image_ctx << ", "
                 << "completion=" << aio_comp << ", off=" << off << ", "
                 << "len=" << len << ", data_len = " << bl.length() << ", "
                 << "flags=" << op_flags << dendl;

  if (native_async && image_ctx.event_socket.is_valid()) {
    aio_comp->set_event_notify(true);
  }

  if (!is_valid_io(image_ctx, aio_comp)) {
    return;
  }

  auto req = io::ImageDispatchSpec<I>::create_write_same(
    image_ctx, io::IMAGE_DISPATCH_LAYER_API_START, aio_comp, off, len,
    std::move(bl), op_flags, trace, 0);
  req->send();
}

template <typename I>
void Io<I>::aio_compare_and_write(I &image_ctx, io::AioCompletion *aio_comp,
                                  uint64_t off, uint64_t len,
                                  bufferlist &&cmp_bl,
                                  bufferlist &&bl, uint64_t *mismatch_off,
                                  int op_flags, bool native_async) {
  auto cct = image_ctx.cct;
  FUNCTRACE(cct);
  ZTracer::Trace trace;
  if (image_ctx.blkin_trace_all) {
    trace.init("io: compare_and_write", &image_ctx.trace_endpoint);
    trace.event("init");
  }

  aio_comp->init_time(util::get_image_ctx(&image_ctx),
                      io::AIO_TYPE_COMPARE_AND_WRITE);
  ldout(cct, 20) << "ictx=" << &image_ctx << ", "
                 << "completion=" << aio_comp << ", off=" << off << ", "
                 << "len=" << len << dendl;

  if (native_async && image_ctx.event_socket.is_valid()) {
    aio_comp->set_event_notify(true);
  }

  if (!is_valid_io(image_ctx, aio_comp)) {
    return;
  }

  auto req = io::ImageDispatchSpec<I>::create_compare_and_write(
    image_ctx, io::IMAGE_DISPATCH_LAYER_API_START, aio_comp, {{off, len}},
    std::move(cmp_bl), std::move(bl), mismatch_off, op_flags, trace, 0);
  req->send();
}

template <typename I>
void Io<I>::aio_flush(I &image_ctx, io::AioCompletion *aio_comp,
                      bool native_async) {
  auto cct = image_ctx.cct;
  FUNCTRACE(cct);
  ZTracer::Trace trace;
  if (image_ctx.blkin_trace_all) {
    trace.init("io: flush", &image_ctx.trace_endpoint);
    trace.event("init");
  }

  aio_comp->init_time(util::get_image_ctx(&image_ctx), io::AIO_TYPE_FLUSH);
  ldout(cct, 20) << "ictx=" << &image_ctx << ", "
                 << "completion=" << aio_comp << dendl;

  if (native_async && image_ctx.event_socket.is_valid()) {
    aio_comp->set_event_notify(true);
  }

  if (!is_valid_io(image_ctx, aio_comp)) {
    return;
  }

  auto req = io::ImageDispatchSpec<I>::create_flush(
    image_ctx, io::IMAGE_DISPATCH_LAYER_API_START, aio_comp,
    io::FLUSH_SOURCE_USER, trace);
  req->send();
}

} // namespace api
} // namespace librbd

template class librbd::api::Io<librbd::ImageCtx>;
