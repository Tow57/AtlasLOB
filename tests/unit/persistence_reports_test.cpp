#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "atlaslob/persistence/inspection.hpp"
#include "atlaslob/persistence/replay.hpp"
#include "reports.hpp"

namespace {

using namespace atlaslob;
using namespace atlaslob::persistence;
using namespace atlaslob::persistence::detail;

[[nodiscard]] Digest256 digest_from(std::uint8_t first) {
  Digest256 result;
  for (std::size_t index = 0U; index < result.bytes.size(); ++index) {
    result.bytes[index] = static_cast<std::uint8_t>(first + index);
  }
  return result;
}

[[nodiscard]] LogHeader header() {
  LogHeader result{
      .format_version = command_log_format_version,
      .semantics_version = atlaslob_semantics_version,
      .log_id = {},
      .first_sequence = domain::Sequence{1U},
      .engine_config =
          {
              .max_total_active_orders = 100U,
          },
      .catalog =
          {
              {
                  .instrument_id = domain::InstrumentId{7U},
                  .max_order_quantity = 1'000U,
                  .tick_increment = domain::PriceTicks{5},
                  .max_active_orders = 16U,
              },
          },
  };
  for (std::size_t index = 0U; index < result.log_id.bytes.size(); ++index) {
    result.log_id.bytes[index] = static_cast<std::uint8_t>(index);
  }
  return result;
}

[[nodiscard]] RecordSummary record_summary() {
  return {
      .offset = 124U,
      .total_length = 102U,
      .payload_length = 36U,
      .record_version = 1U,
      .sequence = domain::Sequence{1U},
      .command_type = domain::CommandType::new_order,
      .outcome = RecordOutcome::committed,
      .rejection_reason = domain::RejectReason::none,
      .event_count = 3U,
      .event_digest = digest_from(0x30U),
  };
}

[[nodiscard]] LogInspectionReport clean_log_report(bool include_records = true) {
  return {
      .header = header(),
      .configuration_digest = digest_from(0x10U),
      .last_sequence = domain::Sequence{1U},
      .header_length = 124U,
      .catalog_length = 28U,
      .input_bytes = 226U,
      .valid_prefix_bytes = 226U,
      .records_scanned = 1U,
      .tail = LogTail::clean,
      .error = {},
      .records = include_records
                     ? std::optional<std::vector<RecordSummary>>{std::vector<RecordSummary>{
                           record_summary()}}
                     : std::nullopt,
  };
}

[[nodiscard]] ReplayReport successful_replay() {
  return {
      .mode = ReplayMode::verify,
      .tail_policy = TailPolicy::strict,
      .tail = ReplayTail::clean,
      .header = header(),
      .last_sequence = domain::Sequence{1U},
      .valid_end_offset = 226U,
      .records_scanned = 1U,
      .records_replayed = 1U,
      .committed = 1U,
      .rejected = 0U,
      .used_valid_prefix = false,
      .warning = {},
      .error = {},
      .divergence = std::nullopt,
      .final_state_digest = digest_from(0x50U),
  };
}

[[nodiscard]] SnapshotRecoveryReport successful_snapshot_recovery() {
  auto replay = successful_replay();
  replay.records_replayed = 0U;
  return {
      .recovery_source = RecoverySource::explicit_snapshot,
      .selected_snapshot = std::nullopt,
      .covered_sequence = domain::Sequence{1U},
      .covered_log_byte_offset = 226U,
      .snapshot_state_digest = digest_from(0x50U),
      .skipped_snapshots = {},
      .snapshot_error = {},
      .replay = std::move(replay),
  };
}

[[nodiscard]] domain::EventHeader event_header(std::uint32_t event_index) {
  return {
      .command_sequence = domain::Sequence{1U},
      .event_index = event_index,
      .instrument_id = domain::InstrumentId{7U},
  };
}

[[nodiscard]] std::vector<domain::Event> every_event_variant() {
  return {
      domain::AcceptedEvent{
          .header = event_header(0U),
          .command_type = domain::CommandType::new_order,
      },
      domain::RejectedEvent{
          .header = event_header(1U),
          .command_type = domain::CommandType::cancel,
          .reason = domain::RejectReason::unknown_order_id,
          .order_id = std::nullopt,
      },
      domain::TradeEvent{
          .header = event_header(2U),
          .aggressor_order_id = domain::OrderId{100U},
          .resting_order_id = domain::OrderId{200U},
          .aggressor_client_id = domain::ClientId{11U},
          .resting_client_id = domain::ClientId{12U},
          .aggressor_side = domain::Side::sell,
          .execution_price = domain::PriceTicks{-25},
          .execution_quantity = domain::Quantity{3U},
          .aggressor_remaining = domain::Quantity{6U},
          .resting_remaining = domain::Quantity{0U},
      },
      domain::RestedEvent{
          .header = event_header(3U),
          .order_id = domain::OrderId{300U},
          .client_id = domain::ClientId{13U},
          .side = domain::Side::buy,
          .price = domain::PriceTicks{90},
          .remaining_quantity = domain::Quantity{4U},
      },
      domain::CanceledEvent{
          .header = event_header(4U),
          .order_id = domain::OrderId{400U},
          .canceled_quantity = domain::Quantity{5U},
      },
      domain::ReplacedEvent{
          .header = event_header(5U),
          .old_order_id = domain::OrderId{500U},
          .new_order_id = domain::OrderId{501U},
      },
      domain::DoneEvent{
          .header = event_header(6U),
          .order_id = domain::OrderId{600U},
          .reason = domain::DoneReason::filled,
          .remaining_quantity = domain::Quantity{0U},
      },
      domain::BookChangedEvent{
          .header = event_header(7U),
          .best_bid =
              domain::TopOfBookLevel{
                  .price = domain::PriceTicks{89},
                  .aggregate_quantity = domain::Quantity{10U},
              },
          .best_ask = std::nullopt,
      },
  };
}

constexpr std::string_view log_id_hex{"000102030405060708090a0b0c0d0e0f"};
constexpr std::string_view configuration_digest_hex{
    "101112131415161718191a1b1c1d1e1f"
    "202122232425262728292a2b2c2d2e2f"};
constexpr std::string_view event_digest_hex{
    "303132333435363738393a3b3c3d3e3f"
    "404142434445464748494a4b4c4d4e4f"};
constexpr std::string_view state_digest_hex{
    "505152535455565758595a5b5c5d5e5f"
    "606162636465666768696a6b6c6d6e6f"};

TEST(PersistenceReports, CleanLogJsonMatchesTheFrozenSchemaAndKeyOrder) {
  const auto actual = render_log_report_json(clean_log_report(), LogReportOperation::inspect_log);
  const std::string expected =
      "{\"schema\":\"ATLAS_LOG_REPORT_V1\",\"operation\":\"inspect_log\","
      "\"status\":\"ok\",\"format_version\":\"1\",\"semantics_version\":\"6\","
      "\"log_id\":\"" +
      std::string{log_id_hex} +
      "\",\"first_sequence\":\"1\",\"header_length\":\"124\","
      "\"catalog_length\":\"28\",\"catalog_count\":\"1\","
      "\"configuration_digest\":\"" +
      std::string{configuration_digest_hex} +
      "\",\"records_scanned\":\"1\",\"last_sequence\":\"1\","
      "\"input_bytes\":\"226\",\"valid_prefix_bytes\":\"226\","
      "\"output_bytes\":null,\"tail\":\"clean\",\"warning\":null,"
      "\"error\":null,\"records\":[{\"offset\":\"124\","
      "\"total_length\":\"102\",\"payload_length\":\"36\","
      "\"record_version\":\"1\",\"sequence\":\"1\","
      "\"command_type\":\"new\",\"outcome\":\"committed\","
      "\"rejection_reason\":\"none\",\"event_count\":\"3\","
      "\"event_digest\":\"" +
      std::string{event_digest_hex} + "\"}]}\n";

  EXPECT_EQ(actual, expected);
}

TEST(PersistenceReports, CleanLogTextMatchesTheDeterministicHumanSchema) {
  const auto actual = render_log_report_text(clean_log_report(), LogReportOperation::inspect_log);
  const std::string expected =
      "schema=ATLAS_LOG_REPORT_V1 operation=inspect_log status=ok "
      "format_version=1 semantics_version=6 log_id=" +
      std::string{log_id_hex} +
      " first_sequence=1 header_length=124 catalog_length=28 "
      "catalog_count=1 configuration_digest=" +
      std::string{configuration_digest_hex} +
      " records_scanned=1 last_sequence=1 input_bytes=226 "
      "valid_prefix_bytes=226 output_bytes=null tail=clean warning=null "
      "error=null records=1\n"
      "record offset=124 total_length=102 payload_length=36 "
      "record_version=1 sequence=1 command_type=new outcome=committed "
      "rejection_reason=none event_count=3 event_digest=" +
      std::string{event_digest_hex} + "\n";

  EXPECT_EQ(actual, expected);
}

TEST(PersistenceReports, CleanRepairRefusalIsInvalidWithoutInventingAnError) {
  const auto report = clean_log_report(false);
  const auto json = render_log_report_json(report, LogReportOperation::repair_tail);
  const std::string expected_json =
      "{\"schema\":\"ATLAS_LOG_REPORT_V1\",\"operation\":\"repair_tail\","
      "\"status\":\"invalid\",\"format_version\":\"1\",\"semantics_version\":\"6\","
      "\"log_id\":\"" +
      std::string{log_id_hex} +
      "\",\"first_sequence\":\"1\",\"header_length\":\"124\","
      "\"catalog_length\":\"28\",\"catalog_count\":\"1\","
      "\"configuration_digest\":\"" +
      std::string{configuration_digest_hex} +
      "\",\"records_scanned\":\"1\",\"last_sequence\":\"1\","
      "\"input_bytes\":\"226\",\"valid_prefix_bytes\":\"226\","
      "\"output_bytes\":null,\"tail\":\"clean\",\"warning\":null,"
      "\"error\":null,\"records\":null}\n";
  const std::string expected_text =
      "schema=ATLAS_LOG_REPORT_V1 operation=repair_tail status=invalid "
      "format_version=1 semantics_version=6 log_id=" +
      std::string{log_id_hex} +
      " first_sequence=1 header_length=124 catalog_length=28 "
      "catalog_count=1 configuration_digest=" +
      std::string{configuration_digest_hex} +
      " records_scanned=1 last_sequence=1 input_bytes=226 "
      "valid_prefix_bytes=226 output_bytes=null tail=clean warning=null "
      "error=null records=null\n";

  EXPECT_EQ(json, expected_json);
  EXPECT_EQ(render_log_report_text(report, LogReportOperation::repair_tail), expected_text);
}

TEST(PersistenceReports, TornRepairReportUsesWarningAndNeverMislabelsItAsCorruption) {
  auto report = clean_log_report(false);
  report.input_bytes = 230U;
  report.tail = LogTail::torn;
  report.error = {
      .category = LogErrorCategory::truncated_final_record,
      .byte_offset = 226U,
  };

  const auto json = render_log_report_json(report, LogReportOperation::repair_tail, 226U);
  const std::string expected =
      "{\"schema\":\"ATLAS_LOG_REPORT_V1\",\"operation\":\"repair_tail\","
      "\"status\":\"warning\",\"format_version\":\"1\","
      "\"semantics_version\":\"6\",\"log_id\":\"" +
      std::string{log_id_hex} +
      "\",\"first_sequence\":\"1\",\"header_length\":\"124\","
      "\"catalog_length\":\"28\",\"catalog_count\":\"1\","
      "\"configuration_digest\":\"" +
      std::string{configuration_digest_hex} +
      "\",\"records_scanned\":\"1\",\"last_sequence\":\"1\","
      "\"input_bytes\":\"230\",\"valid_prefix_bytes\":\"226\","
      "\"output_bytes\":\"226\",\"tail\":\"torn\","
      "\"warning\":{\"category\":\"truncated_final_record\","
      "\"offset\":\"226\"},\"error\":null,\"records\":null}\n";

  EXPECT_EQ(json, expected);
  EXPECT_EQ(render_log_report_text(report, LogReportOperation::repair_tail, 226U),
            "schema=ATLAS_LOG_REPORT_V1 operation=repair_tail status=warning "
            "format_version=1 semantics_version=6 log_id=" +
                std::string{log_id_hex} +
                " first_sequence=1 header_length=124 catalog_length=28 "
                "catalog_count=1 configuration_digest=" +
                std::string{configuration_digest_hex} +
                " records_scanned=1 last_sequence=1 input_bytes=230 "
                "valid_prefix_bytes=226 output_bytes=226 tail=torn "
                "warning=truncated_final_record@226 error=null records=null\n");
}

TEST(PersistenceReports, RepairCleanupFailureSurfacesAPathFreeArtifactStatus) {
  auto report = clean_log_report(false);
  report.input_bytes = 230U;
  report.valid_prefix_bytes = 226U;
  report.tail = LogTail::torn;
  report.error = {
      .category = LogErrorCategory::io_failure,
      .byte_offset = 226U,
      .system_error = std::make_error_code(std::errc::permission_denied),
  };

  const auto json =
      render_log_report_json(report, LogReportOperation::repair_tail, std::nullopt, true);
  EXPECT_NE(json.find("\"status\":\"io_error\""), std::string::npos);
  EXPECT_NE(json.find("\"output_bytes\":null,"
                      "\"unpublished_artifact_present\":true,\"tail\":\"torn\""),
            std::string::npos);
  EXPECT_EQ(json.find("permission_denied"), std::string::npos);

  const auto text =
      render_log_report_text(report, LogReportOperation::repair_tail, std::nullopt, true);
  EXPECT_NE(text.find("status=io_error"), std::string::npos);
  EXPECT_NE(text.find("output_bytes=null unpublished_artifact_present=true tail=torn"),
            std::string::npos);
}

TEST(PersistenceReports, CorruptionUsesTheErrorSlotAndOmitsSystemDetails) {
  auto report = clean_log_report(false);
  report.last_sequence = std::nullopt;
  report.records_scanned = 0U;
  report.valid_prefix_bytes = 124U;
  report.tail = LogTail::unknown;
  report.error = {
      .category = LogErrorCategory::bad_record_checksum,
      .byte_offset = 222U,
      .system_error = std::make_error_code(std::errc::permission_denied),
  };

  const auto json = render_log_report_json(report, LogReportOperation::inspect_log);
  const std::string expected =
      "{\"schema\":\"ATLAS_LOG_REPORT_V1\",\"operation\":\"inspect_log\","
      "\"status\":\"invalid\",\"format_version\":\"1\","
      "\"semantics_version\":\"6\",\"log_id\":\"" +
      std::string{log_id_hex} +
      "\",\"first_sequence\":\"1\",\"header_length\":\"124\","
      "\"catalog_length\":\"28\",\"catalog_count\":\"1\","
      "\"configuration_digest\":\"" +
      std::string{configuration_digest_hex} +
      "\",\"records_scanned\":\"0\",\"last_sequence\":null,"
      "\"input_bytes\":\"226\",\"valid_prefix_bytes\":\"124\","
      "\"output_bytes\":null,\"tail\":\"unknown\",\"warning\":null,"
      "\"error\":{\"category\":\"bad_record_checksum\","
      "\"offset\":\"222\"},\"records\":null}\n";

  EXPECT_EQ(json, expected);
  EXPECT_EQ(json.find("permission"), std::string::npos);
  EXPECT_EQ(json.find("\"path\""), std::string::npos);
  EXPECT_EQ(json.find("elapsed"), std::string::npos);
  EXPECT_EQ(json.find("timestamp"), std::string::npos);
}

TEST(PersistenceReports, VerifiedReplayJsonAndTextMatchTheirFrozenSchemas) {
  const auto report = successful_replay();
  const auto json = render_replay_report_json(report);
  const std::string expected_json =
      "{\"schema\":\"ATLAS_REPLAY_REPORT_V1\",\"status\":\"ok\","
      "\"mode\":\"verify\",\"tail_policy\":\"strict\","
      "\"semantics_version\":\"6\",\"log_id\":\"" +
      std::string{log_id_hex} +
      "\",\"first_sequence\":\"1\",\"last_sequence\":\"1\","
      "\"records_available\":\"1\",\"records_replayed\":\"1\","
      "\"committed\":\"1\",\"rejected\":\"0\","
      "\"final_state_digest\":\"" +
      std::string{state_digest_hex} +
      "\",\"tail\":\"clean\",\"warning\":null,\"error\":null,"
      "\"divergence\":null}\n";
  const std::string expected_text =
      "schema=ATLAS_REPLAY_REPORT_V1 status=ok mode=verify "
      "tail_policy=strict semantics_version=6 log_id=" +
      std::string{log_id_hex} +
      " first_sequence=1 last_sequence=1 records_available=1 "
      "records_replayed=1 committed=1 rejected=0 final_state_digest=" +
      std::string{state_digest_hex} + " tail=clean warning=null error=null divergence=null\n";

  EXPECT_EQ(json, expected_json);
  EXPECT_EQ(render_replay_report_text(report), expected_text);
}

TEST(PersistenceReports, DivergenceRendersCommandEngineErrorAndEveryActualEventExactly) {
  auto report = successful_replay();
  report.final_state_digest = std::nullopt;
  report.divergence = ReplayDivergence{
      .record_offset = 124U,
      .sequence = domain::Sequence{1U},
      .category = ReplayDivergenceCategory::event_digest,
      .command =
          domain::NewOrder{
              .client_id = domain::ClientId{11U},
              .order_id = domain::OrderId{100U},
              .instrument_id = domain::InstrumentId{7U},
              .side = domain::Side::buy,
              .order_type = domain::OrderType::limit,
              .time_in_force = domain::TimeInForce::gtc,
              .limit_price = domain::PriceTicks{-50},
              .quantity = domain::Quantity{9U},
          },
      .expected =
          {
              .outcome = RecordOutcome::committed,
              .rejection_reason = domain::RejectReason::none,
              .event_count = 8U,
              .event_digest = digest_from(0x30U),
          },
      .actual =
          {
              .outcome = RecordOutcome::committed,
              .rejection_reason = domain::RejectReason::none,
              .event_count = 8U,
              .event_digest = digest_from(0x40U),
          },
      .actual_engine_error = EngineError::none,
      .actual_events = every_event_variant(),
  };

  const auto json = render_replay_report_json(report);
  const std::string expected =
      "{\"schema\":\"ATLAS_REPLAY_REPORT_V1\",\"status\":\"diverged\","
      "\"mode\":\"verify\",\"tail_policy\":\"strict\","
      "\"semantics_version\":\"6\",\"log_id\":\"" +
      std::string{log_id_hex} +
      "\",\"first_sequence\":\"1\",\"last_sequence\":\"1\","
      "\"records_available\":\"1\",\"records_replayed\":\"1\","
      "\"committed\":\"1\",\"rejected\":\"0\","
      "\"final_state_digest\":null,\"tail\":\"clean\","
      "\"warning\":null,\"error\":null,\"divergence\":{"
      "\"sequence\":\"1\",\"record_offset\":\"124\","
      "\"category\":\"event_digest\",\"command\":{\"type\":\"new\","
      "\"client_id\":\"11\",\"order_id\":\"100\",\"instrument_id\":\"7\","
      "\"side\":\"buy\",\"side_raw\":\"1\",\"order_type\":\"limit\","
      "\"order_type_raw\":\"1\",\"time_in_force\":\"gtc\","
      "\"time_in_force_raw\":\"1\",\"limit_price\":\"-50\",\"quantity\":\"9\"},"
      "\"expected\":{\"outcome\":\"committed\","
      "\"rejection_reason\":\"none\",\"event_count\":\"8\","
      "\"event_digest\":\"" +
      std::string{event_digest_hex} +
      "\"},\"actual\":{\"outcome\":\"committed\","
      "\"rejection_reason\":\"none\",\"event_count\":\"8\","
      "\"event_digest\":\"" +
      digest_from(0x40U).hex() +
      "\"},\"actual_engine_error\":\"none\",\"actual_events\":["
      "{\"type\":\"accepted\",\"command_sequence\":\"1\",\"event_index\":\"0\","
      "\"instrument_id\":\"7\",\"command_type\":\"new\"},"
      "{\"type\":\"rejected\",\"command_sequence\":\"1\",\"event_index\":\"1\","
      "\"instrument_id\":\"7\",\"command_type\":\"cancel\","
      "\"reason\":\"unknown_order_id\",\"order_id\":null},"
      "{\"type\":\"trade\",\"command_sequence\":\"1\",\"event_index\":\"2\","
      "\"instrument_id\":\"7\",\"aggressor_order_id\":\"100\","
      "\"resting_order_id\":\"200\",\"aggressor_client_id\":\"11\","
      "\"resting_client_id\":\"12\",\"aggressor_side\":\"sell\","
      "\"execution_price\":\"-25\",\"execution_quantity\":\"3\","
      "\"aggressor_remaining\":\"6\",\"resting_remaining\":\"0\"},"
      "{\"type\":\"rested\",\"command_sequence\":\"1\",\"event_index\":\"3\","
      "\"instrument_id\":\"7\",\"order_id\":\"300\",\"client_id\":\"13\","
      "\"side\":\"buy\",\"price\":\"90\",\"remaining_quantity\":\"4\"},"
      "{\"type\":\"canceled\",\"command_sequence\":\"1\",\"event_index\":\"4\","
      "\"instrument_id\":\"7\",\"order_id\":\"400\",\"canceled_quantity\":\"5\"},"
      "{\"type\":\"replaced\",\"command_sequence\":\"1\",\"event_index\":\"5\","
      "\"instrument_id\":\"7\",\"old_order_id\":\"500\",\"new_order_id\":\"501\"},"
      "{\"type\":\"done\",\"command_sequence\":\"1\",\"event_index\":\"6\","
      "\"instrument_id\":\"7\",\"order_id\":\"600\",\"reason\":\"filled\","
      "\"remaining_quantity\":\"0\"},"
      "{\"type\":\"book_changed\",\"command_sequence\":\"1\",\"event_index\":\"7\","
      "\"instrument_id\":\"7\",\"best_bid\":{\"price\":\"89\","
      "\"aggregate_quantity\":\"10\"},\"best_ask\":null}]}}\n";

  EXPECT_EQ(json, expected);
  EXPECT_EQ(render_replay_report_text(report),
            "schema=ATLAS_REPLAY_REPORT_V1 status=diverged mode=verify "
            "tail_policy=strict semantics_version=6 log_id=" +
                std::string{log_id_hex} +
                " first_sequence=1 last_sequence=1 records_available=1 "
                "records_replayed=1 committed=1 rejected=0 "
                "final_state_digest=null tail=clean warning=null error=null "
                "divergence=event_digest\n"
                "divergence sequence=1 record_offset=124 category=event_digest "
                "expected.outcome=committed expected.rejection_reason=none "
                "expected.event_count=8 expected.event_digest=" +
                std::string{event_digest_hex} +
                " actual.outcome=committed actual.rejection_reason=none "
                "actual.event_count=8 actual.event_digest=" +
                digest_from(0x40U).hex() +
                " actual_engine_error=none actual_events=8\n"
                "command type=new client_id=11 order_id=100 instrument_id=7 "
                "side=buy side_raw=1 order_type=limit order_type_raw=1 "
                "time_in_force=gtc time_in_force_raw=1 limit_price=-50 quantity=9\n"
                "actual_event type=accepted command_sequence=1 event_index=0 "
                "instrument_id=7 command_type=new\n"
                "actual_event type=rejected command_sequence=1 event_index=1 "
                "instrument_id=7 command_type=cancel reason=unknown_order_id order_id=null\n"
                "actual_event type=trade command_sequence=1 event_index=2 instrument_id=7 "
                "aggressor_order_id=100 resting_order_id=200 aggressor_client_id=11 "
                "resting_client_id=12 aggressor_side=sell execution_price=-25 "
                "execution_quantity=3 aggressor_remaining=6 resting_remaining=0\n"
                "actual_event type=rested command_sequence=1 event_index=3 instrument_id=7 "
                "order_id=300 client_id=13 side=buy price=90 remaining_quantity=4\n"
                "actual_event type=canceled command_sequence=1 event_index=4 "
                "instrument_id=7 order_id=400 canceled_quantity=5\n"
                "actual_event type=replaced command_sequence=1 event_index=5 "
                "instrument_id=7 old_order_id=500 new_order_id=501\n"
                "actual_event type=done command_sequence=1 event_index=6 instrument_id=7 "
                "order_id=600 reason=filled remaining_quantity=0\n"
                "actual_event type=book_changed command_sequence=1 event_index=7 "
                "instrument_id=7 best_bid.price=89 best_bid.aggregate_quantity=10 "
                "best_ask=null\n");
}

TEST(PersistenceReports, EngineErrorDivergenceRendersTheCancelCommandExactly) {
  auto report = successful_replay();
  report.final_state_digest = std::nullopt;
  report.divergence = ReplayDivergence{
      .record_offset = 124U,
      .sequence = domain::Sequence{1U},
      .category = ReplayDivergenceCategory::engine_error,
      .command =
          domain::CancelOrder{
              .client_id = domain::ClientId{21U},
              .order_id = domain::OrderId{900U},
              .instrument_id = domain::InstrumentId{7U},
          },
      .expected = {},
      .actual = {},
      .actual_engine_error = EngineError::sequence_exhausted,
      .actual_events = {},
  };

  const auto json = render_replay_report_json(report);
  const std::string expected_json =
      "{\"schema\":\"ATLAS_REPLAY_REPORT_V1\",\"status\":\"diverged\","
      "\"mode\":\"verify\",\"tail_policy\":\"strict\","
      "\"semantics_version\":\"6\",\"log_id\":\"" +
      std::string{log_id_hex} +
      "\",\"first_sequence\":\"1\",\"last_sequence\":\"1\","
      "\"records_available\":\"1\",\"records_replayed\":\"1\","
      "\"committed\":\"1\",\"rejected\":\"0\",\"final_state_digest\":null,"
      "\"tail\":\"clean\",\"warning\":null,\"error\":null,\"divergence\":{"
      "\"sequence\":\"1\",\"record_offset\":\"124\",\"category\":\"engine_error\","
      "\"command\":{\"type\":\"cancel\",\"client_id\":\"21\",\"order_id\":\"900\","
      "\"instrument_id\":\"7\"},\"expected\":{\"outcome\":null,"
      "\"rejection_reason\":null,\"event_count\":null,\"event_digest\":null},"
      "\"actual\":{\"outcome\":null,\"rejection_reason\":null,"
      "\"event_count\":null,\"event_digest\":null},"
      "\"actual_engine_error\":\"sequence_exhausted\",\"actual_events\":[]}}\n";
  const std::string expected_text =
      "schema=ATLAS_REPLAY_REPORT_V1 status=diverged mode=verify "
      "tail_policy=strict semantics_version=6 log_id=" +
      std::string{log_id_hex} +
      " first_sequence=1 last_sequence=1 records_available=1 "
      "records_replayed=1 committed=1 rejected=0 final_state_digest=null "
      "tail=clean warning=null error=null divergence=engine_error\n"
      "divergence sequence=1 record_offset=124 category=engine_error "
      "expected.outcome=null expected.rejection_reason=null "
      "expected.event_count=null expected.event_digest=null "
      "actual.outcome=null actual.rejection_reason=null "
      "actual.event_count=null actual.event_digest=null "
      "actual_engine_error=sequence_exhausted actual_events=0\n"
      "command type=cancel client_id=21 order_id=900 instrument_id=7\n";

  EXPECT_EQ(json, expected_json);
  EXPECT_EQ(render_replay_report_text(report), expected_text);
}

TEST(PersistenceReports, ReplaceAndRawNewCommandsHaveStableDiagnosticEncodings) {
  auto report = successful_replay();
  report.final_state_digest = std::nullopt;
  report.divergence = ReplayDivergence{
      .record_offset = 124U,
      .sequence = domain::Sequence{1U},
      .category = ReplayDivergenceCategory::event_count,
      .command =
          domain::ReplaceOrder{
              .client_id = domain::ClientId{31U},
              .old_order_id = domain::OrderId{901U},
              .new_order_id = domain::OrderId{902U},
              .instrument_id = domain::InstrumentId{7U},
              .new_limit_price = domain::PriceTicks{-101},
              .new_quantity = domain::Quantity{15U},
          },
      .expected = {},
      .actual = {},
      .actual_engine_error = EngineError::internal_failure,
      .actual_events = {},
  };

  auto json = render_replay_report_json(report);
  constexpr std::string_view replace_json{
      "\"command\":{\"type\":\"replace\",\"client_id\":\"31\","
      "\"old_order_id\":\"901\",\"new_order_id\":\"902\",\"instrument_id\":\"7\","
      "\"new_limit_price\":\"-101\",\"new_quantity\":\"15\"},\"expected\":"};
  constexpr std::string_view replace_text{
      "command type=replace client_id=31 old_order_id=901 new_order_id=902 "
      "instrument_id=7 new_limit_price=-101 new_quantity=15\n"};

  EXPECT_NE(json.find(replace_json), std::string::npos);
  EXPECT_NE(render_replay_report_text(report).find(replace_text), std::string::npos);

  report.divergence->command = domain::NewOrder{
      .client_id = domain::ClientId{41U},
      .order_id = domain::OrderId{903U},
      .instrument_id = domain::InstrumentId{7U},
      .side = static_cast<domain::Side>(250U),
      .order_type = static_cast<domain::OrderType>(251U),
      .time_in_force = static_cast<domain::TimeInForce>(252U),
      .limit_price = std::nullopt,
      .quantity = domain::Quantity{16U},
  };
  json = render_replay_report_json(report);
  constexpr std::string_view raw_new_json{
      "\"command\":{\"type\":\"new\",\"client_id\":\"41\",\"order_id\":\"903\","
      "\"instrument_id\":\"7\",\"side\":\"unknown\",\"side_raw\":\"250\","
      "\"order_type\":\"unknown\",\"order_type_raw\":\"251\","
      "\"time_in_force\":\"unknown\",\"time_in_force_raw\":\"252\","
      "\"limit_price\":null,\"quantity\":\"16\"},\"expected\":"};
  constexpr std::string_view raw_new_text{
      "command type=new client_id=41 order_id=903 instrument_id=7 "
      "side=unknown side_raw=250 order_type=unknown order_type_raw=251 "
      "time_in_force=unknown time_in_force_raw=252 limit_price=null quantity=16\n"};

  EXPECT_NE(json.find(raw_new_json), std::string::npos);
  EXPECT_NE(render_replay_report_text(report).find(raw_new_text), std::string::npos);
}

TEST(PersistenceReports, FinalInvariantDivergenceHasAnExactNullCommandEncoding) {
  auto report = successful_replay();
  report.final_state_digest = std::nullopt;
  report.divergence = ReplayDivergence{
      .record_offset = 226U,
      .sequence = domain::Sequence{1U},
      .category = ReplayDivergenceCategory::invariant,
      .command = std::nullopt,
      .expected = {},
      .actual = {},
      .actual_engine_error = EngineError::none,
      .actual_events = {},
  };

  const auto json = render_replay_report_json(report);
  const std::string expected_json =
      "{\"schema\":\"ATLAS_REPLAY_REPORT_V1\",\"status\":\"diverged\","
      "\"mode\":\"verify\",\"tail_policy\":\"strict\","
      "\"semantics_version\":\"6\",\"log_id\":\"" +
      std::string{log_id_hex} +
      "\",\"first_sequence\":\"1\",\"last_sequence\":\"1\","
      "\"records_available\":\"1\",\"records_replayed\":\"1\","
      "\"committed\":\"1\",\"rejected\":\"0\",\"final_state_digest\":null,"
      "\"tail\":\"clean\",\"warning\":null,\"error\":null,\"divergence\":{"
      "\"sequence\":\"1\",\"record_offset\":\"226\",\"category\":\"invariant\","
      "\"command\":null,\"expected\":{\"outcome\":null,"
      "\"rejection_reason\":null,\"event_count\":null,\"event_digest\":null},"
      "\"actual\":{\"outcome\":null,\"rejection_reason\":null,"
      "\"event_count\":null,\"event_digest\":null},"
      "\"actual_engine_error\":\"none\",\"actual_events\":[]}}\n";
  const std::string expected_text =
      "schema=ATLAS_REPLAY_REPORT_V1 status=diverged mode=verify "
      "tail_policy=strict semantics_version=6 log_id=" +
      std::string{log_id_hex} +
      " first_sequence=1 last_sequence=1 records_available=1 "
      "records_replayed=1 committed=1 rejected=0 final_state_digest=null "
      "tail=clean warning=null error=null divergence=invariant\n"
      "divergence sequence=1 record_offset=226 category=invariant "
      "expected.outcome=null expected.rejection_reason=null "
      "expected.event_count=null expected.event_digest=null "
      "actual.outcome=null actual.rejection_reason=null "
      "actual.event_count=null actual.event_digest=null "
      "actual_engine_error=none actual_events=0\n"
      "command=null\n";

  EXPECT_EQ(json, expected_json);
  EXPECT_EQ(render_replay_report_text(report), expected_text);
}

TEST(PersistenceReports, ValidPrefixReplayHasAnExplicitWarningAndSuccessfulDigest) {
  auto report = successful_replay();
  report.tail_policy = TailPolicy::valid_prefix;
  report.tail = ReplayTail::torn;
  report.used_valid_prefix = true;
  report.warning = {
      .category = LogErrorCategory::truncated_final_record,
      .byte_offset = 226U,
  };

  const auto json = render_replay_report_json(report);

  EXPECT_NE(json.find("\"status\":\"warning\""), std::string::npos);
  EXPECT_NE(json.find("\"tail_policy\":\"valid-prefix\""), std::string::npos);
  EXPECT_NE(json.find("\"warning\":{\"category\":\"truncated_final_record\","
                      "\"offset\":\"226\"}"),
            std::string::npos);
  EXPECT_NE(json.find("\"final_state_digest\":\"" + std::string{state_digest_hex} + "\""),
            std::string::npos);
  EXPECT_EQ(json.find("\"error\":{\""), std::string::npos);
}

TEST(PersistenceReports, RepeatedVerifiedReportsAreByteIdenticalAndLeakNoAmbientData) {
  const auto first_json = render_replay_report_json(successful_replay());
  const auto second_json = render_replay_report_json(successful_replay());
  const auto first_text = render_replay_report_text(successful_replay());
  const auto second_text = render_replay_report_text(successful_replay());

  EXPECT_EQ(first_json, second_json);
  EXPECT_EQ(first_text, second_text);
  for (const auto& report : {first_json, first_text}) {
    EXPECT_EQ(report.find("C:\\"), std::string::npos);
    EXPECT_EQ(report.find("/home/"), std::string::npos);
    EXPECT_EQ(report.find("elapsed"), std::string::npos);
    EXPECT_EQ(report.find("timestamp"), std::string::npos);
  }
}

TEST(PersistenceReports, SnapshotReplayV2ExplicitSuccessMatchesExactJsonAndText) {
  const auto report = successful_snapshot_recovery();
  const std::string expected_json =
      "{\"schema\":\"ATLAS_REPLAY_REPORT_V2\",\"status\":\"ok\","
      "\"mode\":\"verify\",\"tail_policy\":\"strict\","
      "\"semantics_version\":\"6\",\"log_id\":\"" +
      std::string{log_id_hex} +
      "\",\"first_sequence\":\"1\",\"last_sequence\":\"1\","
      "\"records_available\":\"1\",\"records_covered_by_snapshot\":\"1\","
      "\"records_replayed\":\"0\",\"committed\":\"1\",\"rejected\":\"0\","
      "\"final_state_digest\":\"" +
      std::string{state_digest_hex} +
      "\",\"tail\":\"clean\",\"recovery_source\":\"explicit_snapshot\","
      "\"snapshot\":{\"covered_sequence\":\"1\",\"covered_log_offset\":\"226\","
      "\"state_digest\":\"" +
      std::string{state_digest_hex} +
      "\"},\"skipped_snapshots\":[],\"warnings\":[],\"error\":null,"
      "\"divergence\":null}\n";
  const std::string expected_text =
      "schema=ATLAS_REPLAY_REPORT_V2 status=ok mode=verify tail_policy=strict "
      "records_available=1 records_covered_by_snapshot=1 records_replayed=0 "
      "committed=1 rejected=0 recovery_source=explicit_snapshot "
      "skipped_snapshots=0 final_state_digest=" +
      std::string{state_digest_hex} + " error=null divergence=null\n";

  EXPECT_EQ(render_snapshot_replay_report_json(report), expected_json);
  EXPECT_EQ(render_snapshot_replay_report_text(report), expected_text);
}

TEST(PersistenceReports, SnapshotReplayV2CandidateSkipMatchesExactJsonAndText) {
  auto report = successful_snapshot_recovery();
  report.recovery_source = RecoverySource::directory_snapshot;
  report.skipped_snapshots.push_back({
      .path = {},
      .filename_sequence = domain::Sequence{2U},
      .error =
          {
              .category = SnapshotErrorCategory::bad_checksum,
              .byte_offset = 561U,
          },
  });
  const std::string expected_json =
      "{\"schema\":\"ATLAS_REPLAY_REPORT_V2\",\"status\":\"warning\","
      "\"mode\":\"verify\",\"tail_policy\":\"strict\","
      "\"semantics_version\":\"6\",\"log_id\":\"" +
      std::string{log_id_hex} +
      "\",\"first_sequence\":\"1\",\"last_sequence\":\"1\","
      "\"records_available\":\"1\",\"records_covered_by_snapshot\":\"1\","
      "\"records_replayed\":\"0\",\"committed\":\"1\",\"rejected\":\"0\","
      "\"final_state_digest\":\"" +
      std::string{state_digest_hex} +
      "\",\"tail\":\"clean\",\"recovery_source\":\"directory_snapshot\","
      "\"snapshot\":{\"covered_sequence\":\"1\",\"covered_log_offset\":\"226\","
      "\"state_digest\":\"" +
      std::string{state_digest_hex} +
      "\"},\"skipped_snapshots\":[{\"candidate_sequence\":\"2\","
      "\"category\":\"bad_checksum\",\"offset\":\"561\"}],"
      "\"warnings\":[{\"category\":\"snapshot_candidates_skipped\","
      "\"offset\":null}],\"error\":null,\"divergence\":null}\n";
  const std::string expected_text =
      "schema=ATLAS_REPLAY_REPORT_V2 status=warning mode=verify tail_policy=strict "
      "records_available=1 records_covered_by_snapshot=1 records_replayed=0 "
      "committed=1 rejected=0 recovery_source=directory_snapshot "
      "skipped_snapshots=1 final_state_digest=" +
      std::string{state_digest_hex} +
      " error=null divergence=null\n"
      "skipped_snapshot candidate_sequence=2 category=bad_checksum offset=561\n";

  EXPECT_EQ(render_snapshot_replay_report_json(report), expected_json);
  EXPECT_EQ(render_snapshot_replay_report_text(report), expected_text);
}

TEST(PersistenceReports, SnapshotReplayV2FullLogFallbackMatchesExactJsonAndText) {
  auto report = successful_snapshot_recovery();
  report.recovery_source = RecoverySource::full_log;
  report.covered_sequence.reset();
  report.covered_log_byte_offset.reset();
  report.snapshot_state_digest.reset();
  report.replay.records_replayed = 1U;
  const std::string expected_json =
      "{\"schema\":\"ATLAS_REPLAY_REPORT_V2\",\"status\":\"warning\","
      "\"mode\":\"verify\",\"tail_policy\":\"strict\","
      "\"semantics_version\":\"6\",\"log_id\":\"" +
      std::string{log_id_hex} +
      "\",\"first_sequence\":\"1\",\"last_sequence\":\"1\","
      "\"records_available\":\"1\",\"records_covered_by_snapshot\":\"0\","
      "\"records_replayed\":\"1\",\"committed\":\"1\",\"rejected\":\"0\","
      "\"final_state_digest\":\"" +
      std::string{state_digest_hex} +
      "\",\"tail\":\"clean\",\"recovery_source\":\"full_log\","
      "\"snapshot\":null,\"skipped_snapshots\":[],"
      "\"warnings\":[{\"category\":\"snapshot_fallback_full_log\","
      "\"offset\":null}],\"error\":null,\"divergence\":null}\n";
  const std::string expected_text =
      "schema=ATLAS_REPLAY_REPORT_V2 status=warning mode=verify tail_policy=strict "
      "records_available=1 records_covered_by_snapshot=0 records_replayed=1 "
      "committed=1 rejected=0 recovery_source=full_log skipped_snapshots=0 "
      "final_state_digest=" +
      std::string{state_digest_hex} + " error=null divergence=null\n";

  EXPECT_EQ(render_snapshot_replay_report_json(report), expected_json);
  EXPECT_EQ(render_snapshot_replay_report_text(report), expected_text);
}

TEST(PersistenceReports, SnapshotReplayV2StrictTornFailureMatchesExactJsonAndText) {
  auto report = successful_snapshot_recovery();
  report.covered_sequence.reset();
  report.covered_log_byte_offset.reset();
  report.snapshot_state_digest.reset();
  report.replay.tail = ReplayTail::torn;
  report.replay.final_state_digest.reset();
  report.replay.error = {
      .category = LogErrorCategory::truncated_final_record,
      .byte_offset = 226U,
  };
  const std::string expected_json =
      "{\"schema\":\"ATLAS_REPLAY_REPORT_V2\",\"status\":\"invalid\","
      "\"mode\":\"verify\",\"tail_policy\":\"strict\","
      "\"semantics_version\":\"6\",\"log_id\":\"" +
      std::string{log_id_hex} +
      "\",\"first_sequence\":\"1\",\"last_sequence\":\"1\","
      "\"records_available\":\"1\",\"records_covered_by_snapshot\":\"0\","
      "\"records_replayed\":\"0\",\"committed\":\"1\",\"rejected\":\"0\","
      "\"final_state_digest\":null,\"tail\":\"torn\","
      "\"recovery_source\":\"explicit_snapshot\",\"snapshot\":null,"
      "\"skipped_snapshots\":[],\"warnings\":[],"
      "\"error\":{\"category\":\"truncated_final_record\",\"offset\":\"226\"},"
      "\"divergence\":null}\n";
  const std::string expected_text =
      "schema=ATLAS_REPLAY_REPORT_V2 status=invalid mode=verify tail_policy=strict "
      "records_available=1 records_covered_by_snapshot=0 records_replayed=0 "
      "committed=1 rejected=0 recovery_source=explicit_snapshot "
      "skipped_snapshots=0 final_state_digest=null "
      "error=truncated_final_record@226 divergence=null\n";

  EXPECT_EQ(render_snapshot_replay_report_json(report), expected_json);
  EXPECT_EQ(render_snapshot_replay_report_text(report), expected_text);
}

}  // namespace
