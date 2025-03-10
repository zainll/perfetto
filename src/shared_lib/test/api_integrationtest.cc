/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <thread>

#include "perfetto/public/abi/data_source_abi.h"
#include "perfetto/public/abi/heap_buffer.h"
#include "perfetto/public/abi/pb_decoder_abi.h"
#include "perfetto/public/abi/tracing_session_abi.h"
#include "perfetto/public/abi/track_event_abi.h"
#include "perfetto/public/data_source.h"
#include "perfetto/public/pb_decoder.h"
#include "perfetto/public/producer.h"
#include "perfetto/public/protos/config/trace_config.pzc.h"
#include "perfetto/public/protos/trace/interned_data/interned_data.pzc.h"
#include "perfetto/public/protos/trace/test_event.pzc.h"
#include "perfetto/public/protos/trace/trace.pzc.h"
#include "perfetto/public/protos/trace/trace_packet.pzc.h"
#include "perfetto/public/protos/trace/track_event/debug_annotation.pzc.h"
#include "perfetto/public/protos/trace/track_event/track_descriptor.pzc.h"
#include "perfetto/public/protos/trace/track_event/track_event.pzc.h"
#include "perfetto/public/protos/trace/trigger.pzc.h"
#include "perfetto/public/te_category_macros.h"
#include "perfetto/public/te_macros.h"
#include "perfetto/public/track_event.h"

#include "test/gtest_and_gmock.h"

#include "src/shared_lib/reset_for_testing.h"
#include "src/shared_lib/test/utils.h"

// Tests for the perfetto shared library.

namespace {

using ::perfetto::shlib::test_utils::AllFieldsWithId;
using ::perfetto::shlib::test_utils::FieldView;
using ::perfetto::shlib::test_utils::IdFieldView;
using ::perfetto::shlib::test_utils::MsgField;
using ::perfetto::shlib::test_utils::PbField;
using ::perfetto::shlib::test_utils::StringField;
using ::perfetto::shlib::test_utils::TracingSession;
using ::perfetto::shlib::test_utils::VarIntField;
using ::perfetto::shlib::test_utils::WaitableEvent;
using ::testing::_;
using ::testing::AllOf;
using ::testing::DoAll;
using ::testing::ElementsAre;
using ::testing::InSequence;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::SaveArg;
using ::testing::UnorderedElementsAre;

constexpr char kDataSourceName1[] = "dev.perfetto.example_data_source";
struct PerfettoDs data_source_1 = PERFETTO_DS_INIT();

constexpr char kDataSourceName2[] = "dev.perfetto.example_data_source2";
struct PerfettoDs data_source_2 = PERFETTO_DS_INIT();
void* const kDataSource2UserArg = reinterpret_cast<void*>(0x555);

#define TEST_CATEGORIES(C) \
  C(cat1, "cat1", "") C(cat2, "cat2", "") C(cat3, "cat3", "")
PERFETTO_TE_CATEGORIES_DEFINE(TEST_CATEGORIES)

class MockDs2Callbacks : testing::Mock {
 public:
  MOCK_METHOD(void*,
              OnSetup,
              (struct PerfettoDsImpl*,
               PerfettoDsInstanceIndex inst_id,
               void* ds_config,
               size_t ds_config_size,
               void* user_arg));
  MOCK_METHOD(void,
              OnStart,
              (struct PerfettoDsImpl*,
               PerfettoDsInstanceIndex inst_id,
               void* user_arg,
               void* inst_ctx));
  MOCK_METHOD(void,
              OnStop,
              (struct PerfettoDsImpl*,
               PerfettoDsInstanceIndex inst_id,
               void* user_arg,
               void* inst_ctx,
               struct PerfettoDsOnStopArgs* args));
  MOCK_METHOD(void,
              OnDestroy,
              (struct PerfettoDsImpl*, void* user_arg, void* inst_ctx));
  MOCK_METHOD(void,
              OnFlush,
              (struct PerfettoDsImpl*,
               PerfettoDsInstanceIndex inst_id,
               void* user_arg,
               void* inst_ctx,
               struct PerfettoDsOnFlushArgs* args));
  MOCK_METHOD(void*,
              OnCreateTls,
              (struct PerfettoDsImpl*,
               PerfettoDsInstanceIndex inst_id,
               struct PerfettoDsTracerImpl* tracer,
               void* user_arg));
  MOCK_METHOD(void, OnDeleteTls, (void*));
  MOCK_METHOD(void*,
              OnCreateIncr,
              (struct PerfettoDsImpl*,
               PerfettoDsInstanceIndex inst_id,
               struct PerfettoDsTracerImpl* tracer,
               void* user_arg));
  MOCK_METHOD(void, OnDeleteIncr, (void*));
};

TEST(SharedLibProtobufTest, PerfettoPbDecoderIteratorExample) {
  // # proto-message: perfetto.protos.TestEvent
  // counter: 5
  // payload {
  //   str: "hello"
  //   single_int: -1
  // }
  std::string_view msg =
      "\x18\x05\x2a\x12\x0a\x05\x68\x65\x6c\x6c\x6f\x28\xff\xff\xff\xff\xff\xff"
      "\xff\xff\xff\x01";
  size_t n_counter = 0;
  size_t n_payload = 0;
  size_t n_payload_str = 0;
  size_t n_payload_single_int = 0;
  for (struct PerfettoPbDecoderIterator it =
           PerfettoPbDecoderIterateBegin(msg.data(), msg.size());
       it.field.status != PERFETTO_PB_DECODER_DONE;
       PerfettoPbDecoderIterateNext(&it)) {
    if (it.field.status != PERFETTO_PB_DECODER_OK) {
      ADD_FAILURE() << "Failed to parse main message";
      break;
    }
    switch (it.field.id) {
      case perfetto_protos_TestEvent_counter_field_number:
        n_counter++;
        EXPECT_EQ(it.field.wire_type, PERFETTO_PB_WIRE_TYPE_VARINT);
        {
          uint64_t val = 0;
          EXPECT_TRUE(PerfettoPbDecoderFieldGetUint64(&it.field, &val));
          EXPECT_EQ(val, 5u);
        }
        break;
      case perfetto_protos_TestEvent_payload_field_number:
        n_payload++;
        EXPECT_EQ(it.field.wire_type, PERFETTO_PB_WIRE_TYPE_DELIMITED);
        for (struct PerfettoPbDecoderIterator it2 =
                 PerfettoPbDecoderIterateNestedBegin(it.field.value.delimited);
             it2.field.status != PERFETTO_PB_DECODER_DONE;
             PerfettoPbDecoderIterateNext(&it2)) {
          if (it2.field.status != PERFETTO_PB_DECODER_OK) {
            ADD_FAILURE() << "Failed to parse nested message";
            break;
          }
          switch (it2.field.id) {
            case perfetto_protos_TestEvent_TestPayload_str_field_number:
              n_payload_str++;
              EXPECT_EQ(it2.field.wire_type, PERFETTO_PB_WIRE_TYPE_DELIMITED);
              EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(
                                             it2.field.value.delimited.start),
                                         it2.field.value.delimited.len),
                        "hello");
              break;
            case perfetto_protos_TestEvent_TestPayload_single_int_field_number:
              EXPECT_EQ(it2.field.wire_type, PERFETTO_PB_WIRE_TYPE_VARINT);
              n_payload_single_int++;
              {
                int32_t val = 0;
                EXPECT_TRUE(PerfettoPbDecoderFieldGetInt32(&it2.field, &val));
                EXPECT_EQ(val, -1);
              }
              break;
            default:
              ADD_FAILURE() << "Unexpected nested field.id";
              break;
          }
        }
        break;
      default:
        ADD_FAILURE() << "Unexpected field.id";
        break;
    }
  }
  EXPECT_EQ(n_counter, 1u);
  EXPECT_EQ(n_payload, 1u);
  EXPECT_EQ(n_payload_str, 1u);
  EXPECT_EQ(n_payload_single_int, 1u);
}

class SharedLibDataSourceTest : public testing::Test {
 protected:
  void SetUp() override {
    struct PerfettoProducerInitArgs args = PERFETTO_PRODUCER_INIT_ARGS_INIT();
    args.backends = PERFETTO_BACKEND_IN_PROCESS;
    PerfettoProducerInit(args);
    PerfettoDsRegister(&data_source_1, kDataSourceName1,
                       PerfettoDsParamsDefault());
    RegisterDataSource2();
  }

  void TearDown() override {
    perfetto::shlib::ResetForTesting();
    data_source_1.enabled = &perfetto_atomic_false;
    perfetto::shlib::DsImplDestroy(data_source_1.impl);
    data_source_1.impl = nullptr;
    data_source_2.enabled = &perfetto_atomic_false;
    perfetto::shlib::DsImplDestroy(data_source_2.impl);
    data_source_2.impl = nullptr;
  }

  struct Ds2CustomState {
    void* actual;
    SharedLibDataSourceTest* thiz;
  };

  void RegisterDataSource2() {
    struct PerfettoDsParams params = PerfettoDsParamsDefault();
    params.on_setup_cb = [](struct PerfettoDsImpl* ds_impl,
                            PerfettoDsInstanceIndex inst_id, void* ds_config,
                            size_t ds_config_size, void* user_arg) -> void* {
      auto* thiz = static_cast<SharedLibDataSourceTest*>(user_arg);
      return thiz->ds2_callbacks_.OnSetup(ds_impl, inst_id, ds_config,
                                          ds_config_size, thiz->ds2_user_arg_);
    };
    params.on_start_cb = [](struct PerfettoDsImpl* ds_impl,
                            PerfettoDsInstanceIndex inst_id, void* user_arg,
                            void* inst_ctx) -> void {
      auto* thiz = static_cast<SharedLibDataSourceTest*>(user_arg);
      return thiz->ds2_callbacks_.OnStart(ds_impl, inst_id, thiz->ds2_user_arg_,
                                          inst_ctx);
    };
    params.on_stop_cb =
        [](struct PerfettoDsImpl* ds_impl, PerfettoDsInstanceIndex inst_id,
           void* user_arg, void* inst_ctx, struct PerfettoDsOnStopArgs* args) {
          auto* thiz = static_cast<SharedLibDataSourceTest*>(user_arg);
          return thiz->ds2_callbacks_.OnStop(
              ds_impl, inst_id, thiz->ds2_user_arg_, inst_ctx, args);
        };
    params.on_destroy_cb = [](struct PerfettoDsImpl* ds_impl, void* user_arg,
                              void* inst_ctx) -> void {
      auto* thiz = static_cast<SharedLibDataSourceTest*>(user_arg);
      thiz->ds2_callbacks_.OnDestroy(ds_impl, thiz->ds2_user_arg_, inst_ctx);
    };
    params.on_flush_cb =
        [](struct PerfettoDsImpl* ds_impl, PerfettoDsInstanceIndex inst_id,
           void* user_arg, void* inst_ctx, struct PerfettoDsOnFlushArgs* args) {
          auto* thiz = static_cast<SharedLibDataSourceTest*>(user_arg);
          return thiz->ds2_callbacks_.OnFlush(
              ds_impl, inst_id, thiz->ds2_user_arg_, inst_ctx, args);
        };
    params.on_create_tls_cb =
        [](struct PerfettoDsImpl* ds_impl, PerfettoDsInstanceIndex inst_id,
           struct PerfettoDsTracerImpl* tracer, void* user_arg) -> void* {
      auto* thiz = static_cast<SharedLibDataSourceTest*>(user_arg);
      auto* state = new Ds2CustomState();
      state->thiz = thiz;
      state->actual = thiz->ds2_callbacks_.OnCreateTls(ds_impl, inst_id, tracer,
                                                       thiz->ds2_user_arg_);
      return state;
    };
    params.on_delete_tls_cb = [](void* ptr) {
      auto* state = static_cast<Ds2CustomState*>(ptr);
      state->thiz->ds2_callbacks_.OnDeleteTls(state->actual);
      delete state;
    };
    params.on_create_incr_cb =
        [](struct PerfettoDsImpl* ds_impl, PerfettoDsInstanceIndex inst_id,
           struct PerfettoDsTracerImpl* tracer, void* user_arg) -> void* {
      auto* thiz = static_cast<SharedLibDataSourceTest*>(user_arg);
      auto* state = new Ds2CustomState();
      state->thiz = thiz;
      state->actual = thiz->ds2_callbacks_.OnCreateIncr(
          ds_impl, inst_id, tracer, thiz->ds2_user_arg_);
      return state;
    };
    params.on_delete_incr_cb = [](void* ptr) {
      auto* state = static_cast<Ds2CustomState*>(ptr);
      state->thiz->ds2_callbacks_.OnDeleteIncr(state->actual);
      delete state;
    };
    params.user_arg = this;
    PerfettoDsRegister(&data_source_2, kDataSourceName2, params);
  }

  void* Ds2ActualCustomState(void* ptr) {
    auto* state = static_cast<Ds2CustomState*>(ptr);
    return state->actual;
  }

  NiceMock<MockDs2Callbacks> ds2_callbacks_;
  void* ds2_user_arg_ = kDataSource2UserArg;
};

TEST_F(SharedLibDataSourceTest, DisabledNotExecuted) {
  bool executed = false;

  PERFETTO_DS_TRACE(data_source_1, ctx) {
    executed = true;
  }

  EXPECT_FALSE(executed);
}

TEST_F(SharedLibDataSourceTest, EnabledOnce) {
  size_t executed = 0;
  TracingSession tracing_session =
      TracingSession::Builder().set_data_source_name(kDataSourceName1).Build();

  PERFETTO_DS_TRACE(data_source_1, ctx) {
    executed++;
  }

  EXPECT_EQ(executed, 1u);
}

TEST_F(SharedLibDataSourceTest, EnabledTwice) {
  size_t executed = 0;
  TracingSession tracing_session1 =
      TracingSession::Builder().set_data_source_name(kDataSourceName1).Build();
  TracingSession tracing_session2 =
      TracingSession::Builder().set_data_source_name(kDataSourceName1).Build();

  PERFETTO_DS_TRACE(data_source_1, ctx) {
    executed++;
  }

  EXPECT_EQ(executed, 2u);
}

TEST_F(SharedLibDataSourceTest, Serialization) {
  TracingSession tracing_session =
      TracingSession::Builder().set_data_source_name(kDataSourceName1).Build();

  PERFETTO_DS_TRACE(data_source_1, ctx) {
    struct PerfettoDsRootTracePacket trace_packet;
    PerfettoDsTracerPacketBegin(&ctx, &trace_packet);

    {
      struct perfetto_protos_TestEvent for_testing;
      perfetto_protos_TracePacket_begin_for_testing(&trace_packet.msg,
                                                    &for_testing);
      {
        struct perfetto_protos_TestEvent_TestPayload payload;
        perfetto_protos_TestEvent_begin_payload(&for_testing, &payload);
        perfetto_protos_TestEvent_TestPayload_set_cstr_str(&payload,
                                                           "ABCDEFGH");
        perfetto_protos_TestEvent_end_payload(&for_testing, &payload);
      }
      perfetto_protos_TracePacket_end_for_testing(&trace_packet.msg,
                                                  &for_testing);
    }
    PerfettoDsTracerPacketEnd(&ctx, &trace_packet);
  }
  PERFETTO_DS_TRACE(data_source_1, ctx) {
    struct PerfettoDsRootTracePacket trace_packet;
    PerfettoDsTracerPacketBegin(&ctx, &trace_packet);
    PerfettoDsTracerPacketEnd(&ctx, &trace_packet);
  }

  tracing_session.StopBlocking();
  std::vector<uint8_t> data = tracing_session.ReadBlocking();
  bool found_for_testing = false;
  for (struct PerfettoPbDecoderField trace_field : FieldView(data)) {
    ASSERT_THAT(trace_field, PbField(perfetto_protos_Trace_packet_field_number,
                                     MsgField(_)));
    IdFieldView for_testing(
        trace_field, perfetto_protos_TracePacket_for_testing_field_number);
    ASSERT_TRUE(for_testing.ok());
    if (for_testing.size() == 0) {
      continue;
    }
    found_for_testing = true;
    ASSERT_EQ(for_testing.size(), 1u);
    ASSERT_THAT(FieldView(for_testing.front()),
                ElementsAre(PbField(
                    perfetto_protos_TestEvent_payload_field_number,
                    MsgField(ElementsAre(PbField(
                        perfetto_protos_TestEvent_TestPayload_str_field_number,
                        StringField("ABCDEFGH")))))));
  }
  EXPECT_TRUE(found_for_testing);
}

TEST_F(SharedLibDataSourceTest, Break) {
  TracingSession tracing_session1 =
      TracingSession::Builder().set_data_source_name(kDataSourceName1).Build();
  TracingSession tracing_session2 =
      TracingSession::Builder().set_data_source_name(kDataSourceName1).Build();

  PERFETTO_DS_TRACE(data_source_1, ctx) {
    struct PerfettoDsRootTracePacket trace_packet;
    PerfettoDsTracerPacketBegin(&ctx, &trace_packet);

    {
      struct perfetto_protos_TestEvent for_testing;
      perfetto_protos_TracePacket_begin_for_testing(&trace_packet.msg,
                                                    &for_testing);
      perfetto_protos_TracePacket_end_for_testing(&trace_packet.msg,
                                                  &for_testing);
    }
    PerfettoDsTracerPacketEnd(&ctx, &trace_packet);
    // Break: the packet will be emitted only on the first data source instance
    // and therefore will not show up on `tracing_session2`.
    PERFETTO_DS_TRACE_BREAK(data_source_1, ctx);
  }
  PERFETTO_DS_TRACE(data_source_1, ctx) {
    struct PerfettoDsRootTracePacket trace_packet;
    PerfettoDsTracerPacketBegin(&ctx, &trace_packet);
    PerfettoDsTracerPacketEnd(&ctx, &trace_packet);
  }

  tracing_session1.StopBlocking();
  std::vector<uint8_t> data1 = tracing_session1.ReadBlocking();
  EXPECT_THAT(
      FieldView(data1),
      Contains(PbField(perfetto_protos_Trace_packet_field_number,
                       MsgField(Contains(PbField(
                           perfetto_protos_TracePacket_for_testing_field_number,
                           MsgField(_)))))));
  tracing_session2.StopBlocking();
  std::vector<uint8_t> data2 = tracing_session2.ReadBlocking();
  EXPECT_THAT(
      FieldView(data2),
      Each(PbField(
          perfetto_protos_Trace_packet_field_number,
          MsgField(Not(Contains(PbField(
              perfetto_protos_TracePacket_for_testing_field_number, _)))))));
}

TEST_F(SharedLibDataSourceTest, FlushCb) {
  TracingSession tracing_session =
      TracingSession::Builder().set_data_source_name(kDataSourceName1).Build();
  WaitableEvent notification;

  PERFETTO_DS_TRACE(data_source_1, ctx) {
    PerfettoDsTracerFlush(
        &ctx,
        [](void* p_notification) {
          static_cast<WaitableEvent*>(p_notification)->Notify();
        },
        &notification);
  }

  notification.WaitForNotification();
  EXPECT_TRUE(notification.IsNotified());
}

TEST_F(SharedLibDataSourceTest, LifetimeCallbacks) {
  void* const kInstancePtr = reinterpret_cast<void*>(0x44);
  testing::InSequence seq;
  PerfettoDsInstanceIndex setup_inst, start_inst, stop_inst;
  EXPECT_CALL(ds2_callbacks_, OnSetup(_, _, _, _, kDataSource2UserArg))
      .WillOnce(DoAll(SaveArg<1>(&setup_inst), Return(kInstancePtr)));
  EXPECT_CALL(ds2_callbacks_, OnStart(_, _, kDataSource2UserArg, kInstancePtr))
      .WillOnce(SaveArg<1>(&start_inst));

  TracingSession tracing_session =
      TracingSession::Builder().set_data_source_name(kDataSourceName2).Build();

  EXPECT_CALL(ds2_callbacks_,
              OnStop(_, _, kDataSource2UserArg, kInstancePtr, _))
      .WillOnce(SaveArg<1>(&stop_inst));
  EXPECT_CALL(ds2_callbacks_, OnDestroy(_, kDataSource2UserArg, kInstancePtr));

  tracing_session.StopBlocking();

  EXPECT_EQ(setup_inst, start_inst);
  EXPECT_EQ(setup_inst, stop_inst);
}

TEST_F(SharedLibDataSourceTest, StopDone) {
  TracingSession tracing_session =
      TracingSession::Builder().set_data_source_name(kDataSourceName2).Build();

  WaitableEvent stop_called;
  struct PerfettoDsAsyncStopper* stopper;

  EXPECT_CALL(ds2_callbacks_, OnStop(_, _, kDataSource2UserArg, _, _))
      .WillOnce([&](struct PerfettoDsImpl*, PerfettoDsInstanceIndex, void*,
                    void*, struct PerfettoDsOnStopArgs* args) {
        stopper = PerfettoDsOnStopArgsPostpone(args);
        stop_called.Notify();
      });

  std::thread t([&]() { tracing_session.StopBlocking(); });

  stop_called.WaitForNotification();
  PerfettoDsStopDone(stopper);

  t.join();
}

TEST_F(SharedLibDataSourceTest, FlushDone) {
  TracingSession tracing_session =
      TracingSession::Builder().set_data_source_name(kDataSourceName2).Build();

  WaitableEvent flush_called;
  WaitableEvent flush_done;
  struct PerfettoDsAsyncFlusher* flusher;

  EXPECT_CALL(ds2_callbacks_, OnFlush(_, _, kDataSource2UserArg, _, _))
      .WillOnce([&](struct PerfettoDsImpl*, PerfettoDsInstanceIndex, void*,
                    void*, struct PerfettoDsOnFlushArgs* args) {
        flusher = PerfettoDsOnFlushArgsPostpone(args);
        flush_called.Notify();
      });

  std::thread t([&]() {
    tracing_session.FlushBlocking(/*timeout_ms=*/10000);
    flush_done.Notify();
  });

  flush_called.WaitForNotification();
  EXPECT_FALSE(flush_done.IsNotified());
  PerfettoDsFlushDone(flusher);
  flush_done.WaitForNotification();

  t.join();
}

TEST_F(SharedLibDataSourceTest, ThreadLocalState) {
  bool ignored = false;
  void* const kTlsPtr = &ignored;
  TracingSession tracing_session =
      TracingSession::Builder().set_data_source_name(kDataSourceName2).Build();

  EXPECT_CALL(ds2_callbacks_, OnCreateTls).WillOnce(Return(kTlsPtr));

  void* tls_state = nullptr;
  PERFETTO_DS_TRACE(data_source_2, ctx) {
    tls_state = PerfettoDsGetCustomTls(&data_source_2, &ctx);
  }
  EXPECT_EQ(Ds2ActualCustomState(tls_state), kTlsPtr);

  tracing_session.StopBlocking();

  EXPECT_CALL(ds2_callbacks_, OnDeleteTls(kTlsPtr));

  // The OnDelete callback will be called by
  // DestroyStoppedTraceWritersForCurrentThread(). One way to trigger that is to
  // trace with another data source.
  TracingSession tracing_session_1 =
      TracingSession::Builder().set_data_source_name(kDataSourceName1).Build();
  PERFETTO_DS_TRACE(data_source_1, ctx) {}
}

TEST_F(SharedLibDataSourceTest, IncrementalState) {
  bool ignored = false;
  void* const kIncrPtr = &ignored;
  TracingSession tracing_session =
      TracingSession::Builder().set_data_source_name(kDataSourceName2).Build();

  EXPECT_CALL(ds2_callbacks_, OnCreateIncr).WillOnce(Return(kIncrPtr));

  void* tls_state = nullptr;
  PERFETTO_DS_TRACE(data_source_2, ctx) {
    tls_state = PerfettoDsGetIncrementalState(&data_source_2, &ctx);
  }
  EXPECT_EQ(Ds2ActualCustomState(tls_state), kIncrPtr);

  tracing_session.StopBlocking();

  EXPECT_CALL(ds2_callbacks_, OnDeleteIncr(kIncrPtr));

  // The OnDelete callback will be called by
  // DestroyStoppedTraceWritersForCurrentThread(). One way to trigger that is to
  // trace with another data source.
  TracingSession tracing_session_1 =
      TracingSession::Builder().set_data_source_name(kDataSourceName1).Build();
  PERFETTO_DS_TRACE(data_source_1, ctx) {}
}

class SharedLibProducerTest : public testing::Test {
 protected:
  void SetUp() override {
    struct PerfettoProducerInitArgs args = {0};
    args.backends = PERFETTO_BACKEND_IN_PROCESS;
    PerfettoProducerInit(args);
  }

  void TearDown() override { perfetto::shlib::ResetForTesting(); }
};

TEST_F(SharedLibProducerTest, ActivateTriggers) {
  struct PerfettoPbMsgWriter writer;
  struct PerfettoHeapBuffer* hb = PerfettoHeapBufferCreate(&writer.writer);

  struct perfetto_protos_TraceConfig cfg;
  PerfettoPbMsgInit(&cfg.msg, &writer);
  {
    struct perfetto_protos_TraceConfig_BufferConfig buffers;
    perfetto_protos_TraceConfig_begin_buffers(&cfg, &buffers);
    perfetto_protos_TraceConfig_BufferConfig_set_size_kb(&buffers, 1024);
    perfetto_protos_TraceConfig_end_buffers(&cfg, &buffers);
  }
  {
    struct perfetto_protos_TraceConfig_TriggerConfig trigger_config;
    perfetto_protos_TraceConfig_begin_trigger_config(&cfg, &trigger_config);
    perfetto_protos_TraceConfig_TriggerConfig_set_trigger_mode(
        &trigger_config,
        perfetto_protos_TraceConfig_TriggerConfig_STOP_TRACING);
    perfetto_protos_TraceConfig_TriggerConfig_set_trigger_timeout_ms(
        &trigger_config, 5000);
    {
      struct perfetto_protos_TraceConfig_TriggerConfig_Trigger trigger;
      perfetto_protos_TraceConfig_TriggerConfig_begin_triggers(&trigger_config,
                                                               &trigger);
      perfetto_protos_TraceConfig_TriggerConfig_Trigger_set_cstr_name(
          &trigger, "trigger1");
      perfetto_protos_TraceConfig_TriggerConfig_end_triggers(&trigger_config,
                                                             &trigger);
    }
    perfetto_protos_TraceConfig_end_trigger_config(&cfg, &trigger_config);
  }
  size_t cfg_size = PerfettoStreamWriterGetWrittenSize(&writer.writer);
  std::unique_ptr<uint8_t[]> ser(new uint8_t[cfg_size]);
  PerfettoHeapBufferCopyInto(hb, &writer.writer, ser.get(), cfg_size);
  PerfettoHeapBufferDestroy(hb, &writer.writer);

  struct PerfettoTracingSessionImpl* ts =
      PerfettoTracingSessionCreate(PERFETTO_BACKEND_IN_PROCESS);

  PerfettoTracingSessionSetup(ts, ser.get(), cfg_size);

  PerfettoTracingSessionStartBlocking(ts);
  TracingSession tracing_session = TracingSession::Adopt(ts);

  const char* triggers[3];
  triggers[0] = "trigger0";
  triggers[1] = "trigger1";
  triggers[2] = nullptr;
  PerfettoProducerActivateTriggers(triggers, 10000);

  tracing_session.WaitForStopped();
  std::vector<uint8_t> data = tracing_session.ReadBlocking();
  EXPECT_THAT(FieldView(data),
              Contains(PbField(
                  perfetto_protos_Trace_packet_field_number,
                  MsgField(Contains(PbField(
                      perfetto_protos_TracePacket_trigger_field_number,
                      MsgField(Contains(PbField(
                          perfetto_protos_Trigger_trigger_name_field_number,
                          StringField("trigger1"))))))))));
}

class SharedLibTrackEventTest : public testing::Test {
 protected:
  void SetUp() override {
    struct PerfettoProducerInitArgs args = {0};
    args.backends = PERFETTO_BACKEND_IN_PROCESS;
    PerfettoProducerInit(args);
    PerfettoTeInit();
    PERFETTO_TE_REGISTER_CATEGORIES(TEST_CATEGORIES);
  }

  void TearDown() override {
    PERFETTO_TE_UNREGISTER_CATEGORIES(TEST_CATEGORIES);
    perfetto::shlib::ResetForTesting();
  }
};

TEST_F(SharedLibTrackEventTest, TrackEventFastpathOtherDsCatDisabled) {
  TracingSession tracing_session =
      TracingSession::Builder()
          .set_data_source_name("other_nonexisting_datasource")
          .Build();
  EXPECT_FALSE(std::atomic_load(cat1.enabled));
  EXPECT_FALSE(std::atomic_load(cat2.enabled));
  EXPECT_FALSE(std::atomic_load(cat3.enabled));
}

TEST_F(SharedLibTrackEventTest, TrackEventFastpathEmptyConfigEnableAllCats) {
  ASSERT_FALSE(std::atomic_load(cat1.enabled));
  ASSERT_FALSE(std::atomic_load(cat2.enabled));
  ASSERT_FALSE(std::atomic_load(cat3.enabled));

  TracingSession tracing_session =
      TracingSession::Builder().set_data_source_name("track_event").Build();

  EXPECT_TRUE(std::atomic_load(cat1.enabled));
  EXPECT_TRUE(std::atomic_load(cat2.enabled));
  EXPECT_TRUE(std::atomic_load(cat3.enabled));
}

TEST_F(SharedLibTrackEventTest, TrackEventFastpathOneCatEnabled) {
  ASSERT_FALSE(std::atomic_load(cat1.enabled));
  ASSERT_FALSE(std::atomic_load(cat2.enabled));
  ASSERT_FALSE(std::atomic_load(cat3.enabled));

  TracingSession tracing_session = TracingSession::Builder()
                                       .set_data_source_name("track_event")
                                       .add_enabled_category("cat1")
                                       .add_disabled_category("*")
                                       .Build();

  EXPECT_TRUE(std::atomic_load(cat1.enabled));
  EXPECT_FALSE(std::atomic_load(cat2.enabled));
  EXPECT_FALSE(std::atomic_load(cat3.enabled));
}

TEST_F(SharedLibTrackEventTest, TrackEventHlCategory) {
  TracingSession tracing_session =
      TracingSession::Builder().set_data_source_name("track_event").Build();

  PERFETTO_TE(cat1, PERFETTO_TE_INSTANT(""));
  PERFETTO_TE(cat1, PERFETTO_TE_INSTANT(""));

  tracing_session.StopBlocking();
  std::vector<uint8_t> data = tracing_session.ReadBlocking();
  bool found = false;
  for (struct PerfettoPbDecoderField trace_field : FieldView(data)) {
    ASSERT_THAT(trace_field, PbField(perfetto_protos_Trace_packet_field_number,
                                     MsgField(_)));
    IdFieldView track_event(
        trace_field, perfetto_protos_TracePacket_track_event_field_number);
    if (track_event.size() == 0) {
      continue;
    }
    found = true;
    IdFieldView cat_iid_fields(
        track_event.front(),
        perfetto_protos_TrackEvent_category_iids_field_number);
    ASSERT_THAT(cat_iid_fields, ElementsAre(VarIntField(_)));
    uint64_t cat_iid = cat_iid_fields.front().value.integer64;
    EXPECT_THAT(
        trace_field,
        AllFieldsWithId(
            perfetto_protos_TracePacket_interned_data_field_number,
            ElementsAre(AllFieldsWithId(
                perfetto_protos_InternedData_event_categories_field_number,
                ElementsAre(MsgField(UnorderedElementsAre(
                    PbField(perfetto_protos_EventCategory_iid_field_number,
                            VarIntField(cat_iid)),
                    PbField(perfetto_protos_EventCategory_name_field_number,
                            StringField("cat1")))))))));
  }
  EXPECT_TRUE(found);
}

TEST_F(SharedLibTrackEventTest, TrackEventHlDynamicCategory) {
  TracingSession tracing_session = TracingSession::Builder()
                                       .set_data_source_name("track_event")
                                       .add_enabled_category("dyn1")
                                       .add_enabled_category("cat1")
                                       .add_disabled_category("*")
                                       .Build();

  PERFETTO_TE(PERFETTO_TE_DYNAMIC_CATEGORY, PERFETTO_TE_INSTANT(""),
              PERFETTO_TE_DYNAMIC_CATEGORY_STRING("dyn2"));
  PERFETTO_TE(PERFETTO_TE_DYNAMIC_CATEGORY, PERFETTO_TE_INSTANT(""),
              PERFETTO_TE_DYNAMIC_CATEGORY_STRING("dyn1"));
  PERFETTO_TE(cat1, PERFETTO_TE_INSTANT(""));

  tracing_session.StopBlocking();
  std::vector<uint8_t> data = tracing_session.ReadBlocking();
  bool found = false;
  for (struct PerfettoPbDecoderField trace_field : FieldView(data)) {
    ASSERT_THAT(trace_field, PbField(perfetto_protos_Trace_packet_field_number,
                                     MsgField(_)));
    IdFieldView track_event(
        trace_field, perfetto_protos_TracePacket_track_event_field_number);
    if (track_event.size() == 0) {
      continue;
    }
    found = true;
    EXPECT_THAT(track_event,
                ElementsAre(AllFieldsWithId(
                    perfetto_protos_TrackEvent_categories_field_number,
                    ElementsAre(StringField("dyn1")))));
  }
  EXPECT_TRUE(found);
}

TEST_F(SharedLibTrackEventTest, TrackEventHlInstant) {
  TracingSession tracing_session =
      TracingSession::Builder().set_data_source_name("track_event").Build();

  PERFETTO_TE(cat1, PERFETTO_TE_INSTANT("event"));
  PERFETTO_TE(cat1, PERFETTO_TE_INSTANT(""));

  tracing_session.StopBlocking();
  std::vector<uint8_t> data = tracing_session.ReadBlocking();
  bool found = false;
  for (struct PerfettoPbDecoderField trace_field : FieldView(data)) {
    ASSERT_THAT(trace_field, PbField(perfetto_protos_Trace_packet_field_number,
                                     MsgField(_)));
    IdFieldView track_event(
        trace_field, perfetto_protos_TracePacket_track_event_field_number);
    if (track_event.size() == 0) {
      continue;
    }
    found = true;
    ASSERT_THAT(track_event,
                ElementsAre(AllFieldsWithId(
                    perfetto_protos_TrackEvent_type_field_number,
                    ElementsAre(VarIntField(
                        perfetto_protos_TrackEvent_TYPE_INSTANT)))));
    IdFieldView name_iid_fields(
        track_event.front(), perfetto_protos_TrackEvent_name_iid_field_number);
    ASSERT_THAT(name_iid_fields, ElementsAre(VarIntField(_)));
    uint64_t name_iid = name_iid_fields.front().value.integer64;
    EXPECT_THAT(trace_field,
                AllFieldsWithId(
                    perfetto_protos_TracePacket_interned_data_field_number,
                    ElementsAre(AllFieldsWithId(
                        perfetto_protos_InternedData_event_names_field_number,
                        ElementsAre(MsgField(UnorderedElementsAre(
                            PbField(perfetto_protos_EventName_iid_field_number,
                                    VarIntField(name_iid)),
                            PbField(perfetto_protos_EventName_name_field_number,
                                    StringField("event")))))))));
  }
  EXPECT_TRUE(found);
}

TEST_F(SharedLibTrackEventTest, TrackEventLlInstant) {
  TracingSession tracing_session =
      TracingSession::Builder().set_data_source_name("track_event").Build();

  if (PERFETTO_UNLIKELY(PERFETTO_ATOMIC_LOAD_EXPLICIT(
          cat1.enabled, PERFETTO_MEMORY_ORDER_RELAXED))) {
    struct PerfettoTeTimestamp timestamp = PerfettoTeGetTimestamp();
    int32_t type = PERFETTO_TE_TYPE_INSTANT;
    const char* name = "event";
    for (struct PerfettoTeLlIterator ctx =
             PerfettoTeLlBeginSlowPath(&cat1, timestamp);
         ctx.impl.ds.tracer != nullptr;
         PerfettoTeLlNext(&cat1, timestamp, &ctx)) {
      uint64_t name_iid;
      {
        struct PerfettoDsRootTracePacket trace_packet;
        PerfettoTeLlPacketBegin(&ctx, &trace_packet);
        PerfettoTeLlWriteTimestamp(&trace_packet.msg, &timestamp);
        perfetto_protos_TracePacket_set_sequence_flags(
            &trace_packet.msg,
            perfetto_protos_TracePacket_SEQ_NEEDS_INCREMENTAL_STATE);
        {
          struct PerfettoTeLlInternContext intern_ctx;
          PerfettoTeLlInternContextInit(&intern_ctx, ctx.impl.incr,
                                        &trace_packet.msg);
          PerfettoTeLlInternRegisteredCat(&intern_ctx, &cat1);
          name_iid = PerfettoTeLlInternEventName(&intern_ctx, name);
          PerfettoTeLlInternContextDestroy(&intern_ctx);
        }
        {
          struct perfetto_protos_TrackEvent te_msg;
          perfetto_protos_TracePacket_begin_track_event(&trace_packet.msg,
                                                        &te_msg);
          perfetto_protos_TrackEvent_set_type(
              &te_msg, static_cast<enum perfetto_protos_TrackEvent_Type>(type));
          PerfettoTeLlWriteRegisteredCat(&te_msg, &cat1);
          PerfettoTeLlWriteInternedEventName(&te_msg, name_iid);
          perfetto_protos_TracePacket_end_track_event(&trace_packet.msg,
                                                      &te_msg);
        }
        PerfettoTeLlPacketEnd(&ctx, &trace_packet);
      }
    }
  }
  PERFETTO_TE(cat1, PERFETTO_TE_INSTANT(""));

  tracing_session.StopBlocking();
  std::vector<uint8_t> data = tracing_session.ReadBlocking();
  bool found = false;
  for (struct PerfettoPbDecoderField trace_field : FieldView(data)) {
    ASSERT_THAT(trace_field, PbField(perfetto_protos_Trace_packet_field_number,
                                     MsgField(_)));
    IdFieldView track_event(
        trace_field, perfetto_protos_TracePacket_track_event_field_number);
    if (track_event.size() == 0) {
      continue;
    }
    found = true;
    ASSERT_THAT(track_event,
                ElementsAre(AllFieldsWithId(
                    perfetto_protos_TrackEvent_type_field_number,
                    ElementsAre(VarIntField(
                        perfetto_protos_TrackEvent_TYPE_INSTANT)))));
    IdFieldView name_iid_fields(
        track_event.front(), perfetto_protos_TrackEvent_name_iid_field_number);
    ASSERT_THAT(name_iid_fields, ElementsAre(VarIntField(_)));
    uint64_t name_iid = name_iid_fields.front().value.integer64;
    EXPECT_THAT(trace_field,
                AllFieldsWithId(
                    perfetto_protos_TracePacket_interned_data_field_number,
                    ElementsAre(AllFieldsWithId(
                        perfetto_protos_InternedData_event_names_field_number,
                        ElementsAre(MsgField(UnorderedElementsAre(
                            PbField(perfetto_protos_EventName_iid_field_number,
                                    VarIntField(name_iid)),
                            PbField(perfetto_protos_EventName_name_field_number,
                                    StringField("event")))))))));
  }
  EXPECT_TRUE(found);
}

TEST_F(SharedLibTrackEventTest, TrackEventHlInstantNoIntern) {
  TracingSession tracing_session =
      TracingSession::Builder().set_data_source_name("track_event").Build();

  PERFETTO_TE(cat1, PERFETTO_TE_INSTANT("event"), PERFETTO_TE_NO_INTERN());
  PERFETTO_TE(cat1, PERFETTO_TE_INSTANT(""));

  tracing_session.StopBlocking();
  std::vector<uint8_t> data = tracing_session.ReadBlocking();
  bool found = false;
  for (struct PerfettoPbDecoderField trace_field : FieldView(data)) {
    ASSERT_THAT(trace_field, PbField(perfetto_protos_Trace_packet_field_number,
                                     MsgField(_)));
    IdFieldView track_event(
        trace_field, perfetto_protos_TracePacket_track_event_field_number);
    if (track_event.size() == 0) {
      continue;
    }
    found = true;
    ASSERT_THAT(
        track_event,
        ElementsAre(AllOf(
            AllFieldsWithId(perfetto_protos_TrackEvent_type_field_number,
                            ElementsAre(VarIntField(
                                perfetto_protos_TrackEvent_TYPE_INSTANT))),
            AllFieldsWithId(perfetto_protos_TrackEvent_name_field_number,
                            ElementsAre(StringField("event"))))));
  }
  EXPECT_TRUE(found);
}

TEST_F(SharedLibTrackEventTest, TrackEventHlDbgArg) {
  TracingSession tracing_session =
      TracingSession::Builder().set_data_source_name("track_event").Build();

  PERFETTO_TE(cat1, PERFETTO_TE_INSTANT("event"),
              PERFETTO_TE_ARG_UINT64("arg_name", 42));
  PERFETTO_TE(cat1, PERFETTO_TE_INSTANT(""));

  tracing_session.StopBlocking();
  std::vector<uint8_t> data = tracing_session.ReadBlocking();
  bool found = false;
  for (struct PerfettoPbDecoderField trace_field : FieldView(data)) {
    ASSERT_THAT(trace_field, PbField(perfetto_protos_Trace_packet_field_number,
                                     MsgField(_)));
    IdFieldView track_event(
        trace_field, perfetto_protos_TracePacket_track_event_field_number);
    if (track_event.size() == 0) {
      continue;
    }
    found = true;
    ASSERT_THAT(track_event,
                ElementsAre(AllFieldsWithId(
                    perfetto_protos_TrackEvent_type_field_number,
                    ElementsAre(VarIntField(
                        perfetto_protos_TrackEvent_TYPE_INSTANT)))));
    IdFieldView name_iid_fields(
        track_event.front(), perfetto_protos_TrackEvent_name_iid_field_number);
    ASSERT_THAT(name_iid_fields, ElementsAre(VarIntField(_)));
    uint64_t name_iid = name_iid_fields.front().value.integer64;
    IdFieldView debug_annot_fields(
        track_event.front(),
        perfetto_protos_TrackEvent_debug_annotations_field_number);
    ASSERT_THAT(
        debug_annot_fields,
        ElementsAre(MsgField(UnorderedElementsAre(
            PbField(perfetto_protos_DebugAnnotation_name_iid_field_number,
                    VarIntField(_)),
            PbField(perfetto_protos_DebugAnnotation_uint_value_field_number,
                    VarIntField(42))))));
    uint64_t arg_name_iid =
        IdFieldView(debug_annot_fields.front(),
                    perfetto_protos_DebugAnnotation_name_iid_field_number)
            .front()
            .value.integer64;
    EXPECT_THAT(
        trace_field,
        AllFieldsWithId(
            perfetto_protos_TracePacket_interned_data_field_number,
            ElementsAre(AllOf(
                AllFieldsWithId(
                    perfetto_protos_InternedData_event_names_field_number,
                    ElementsAre(MsgField(UnorderedElementsAre(
                        PbField(perfetto_protos_EventName_iid_field_number,
                                VarIntField(name_iid)),
                        PbField(perfetto_protos_EventName_name_field_number,
                                StringField("event")))))),
                AllFieldsWithId(
                    perfetto_protos_InternedData_debug_annotation_names_field_number,
                    ElementsAre(MsgField(UnorderedElementsAre(
                        PbField(
                            perfetto_protos_DebugAnnotationName_iid_field_number,
                            VarIntField(arg_name_iid)),
                        PbField(
                            perfetto_protos_DebugAnnotationName_name_field_number,
                            StringField("arg_name"))))))))));
  }
  EXPECT_TRUE(found);
}

TEST_F(SharedLibTrackEventTest, TrackEventHlNamedTrack) {
  TracingSession tracing_session =
      TracingSession::Builder().set_data_source_name("track_event").Build();

  PERFETTO_TE(cat1, PERFETTO_TE_INSTANT("event"),
              PERFETTO_TE_NAMED_TRACK("MyTrack", 1, 2));
  PERFETTO_TE(cat1, PERFETTO_TE_INSTANT(""));

  uint64_t kExpectedUuid = PerfettoTeNamedTrackUuid("MyTrack", 1, 2);

  tracing_session.StopBlocking();
  std::vector<uint8_t> data = tracing_session.ReadBlocking();
  EXPECT_THAT(
      FieldView(data),
      AllOf(
          Contains(PbField(
              perfetto_protos_Trace_packet_field_number,
              AllFieldsWithId(
                  perfetto_protos_TracePacket_track_descriptor_field_number,
                  ElementsAre(MsgField(UnorderedElementsAre(
                      PbField(perfetto_protos_TrackDescriptor_uuid_field_number,
                              VarIntField(kExpectedUuid)),
                      PbField(perfetto_protos_TrackDescriptor_name_field_number,
                              StringField("MyTrack")),
                      PbField(
                          perfetto_protos_TrackDescriptor_parent_uuid_field_number,
                          VarIntField(2)))))))),
          Contains(PbField(
              perfetto_protos_Trace_packet_field_number,
              AllFieldsWithId(
                  perfetto_protos_TracePacket_track_event_field_number,
                  ElementsAre(AllOf(
                      AllFieldsWithId(
                          perfetto_protos_TrackEvent_type_field_number,
                          ElementsAre(VarIntField(
                              perfetto_protos_TrackEvent_TYPE_INSTANT))),
                      AllFieldsWithId(
                          perfetto_protos_TrackEvent_track_uuid_field_number,
                          ElementsAre(VarIntField(kExpectedUuid))))))))));
}

TEST_F(SharedLibTrackEventTest, TrackEventHlRegisteredCounter) {
  TracingSession tracing_session =
      TracingSession::Builder().set_data_source_name("track_event").Build();

  PerfettoTeRegisteredTrack my_counter_track;
  PerfettoTeCounterTrackRegister(&my_counter_track, "MyCounter",
                                 PerfettoTeProcessTrackUuid());

  PERFETTO_TE(cat1, PERFETTO_TE_COUNTER(),
              PERFETTO_TE_REGISTERED_TRACK(&my_counter_track),
              PERFETTO_TE_INT_COUNTER(42));
  PERFETTO_TE(cat1, PERFETTO_TE_INSTANT(""));

  PerfettoTeRegisteredTrackUnregister(&my_counter_track);

  uint64_t kExpectedUuid =
      PerfettoTeCounterTrackUuid("MyCounter", PerfettoTeProcessTrackUuid());

  tracing_session.StopBlocking();
  std::vector<uint8_t> data = tracing_session.ReadBlocking();
  EXPECT_THAT(
      FieldView(data),
      AllOf(
          Contains(PbField(
              perfetto_protos_Trace_packet_field_number,
              AllFieldsWithId(
                  perfetto_protos_TracePacket_track_descriptor_field_number,
                  ElementsAre(MsgField(UnorderedElementsAre(
                      PbField(perfetto_protos_TrackDescriptor_uuid_field_number,
                              VarIntField(kExpectedUuid)),
                      PbField(perfetto_protos_TrackDescriptor_name_field_number,
                              StringField("MyCounter")),
                      PbField(
                          perfetto_protos_TrackDescriptor_parent_uuid_field_number,
                          VarIntField(PerfettoTeProcessTrackUuid())),
                      PbField(
                          perfetto_protos_TrackDescriptor_counter_field_number,
                          MsgField(_)))))))),
          Contains(PbField(
              perfetto_protos_Trace_packet_field_number,
              AllFieldsWithId(
                  perfetto_protos_TracePacket_track_event_field_number,
                  ElementsAre(AllOf(
                      AllFieldsWithId(
                          perfetto_protos_TrackEvent_type_field_number,
                          ElementsAre(VarIntField(
                              perfetto_protos_TrackEvent_TYPE_COUNTER))),
                      AllFieldsWithId(
                          perfetto_protos_TrackEvent_counter_value_field_number,
                          ElementsAre(VarIntField(42))),
                      AllFieldsWithId(
                          perfetto_protos_TrackEvent_track_uuid_field_number,
                          ElementsAre(VarIntField(kExpectedUuid))))))))));
}

}  // namespace
