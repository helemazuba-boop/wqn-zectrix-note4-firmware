// Production bodies, real temporary files, injectable libc errors; codec stub.
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_INVALID_ARG = 0x102, ESP_ERR_NOT_FOUND = 0x105;
constexpr esp_err_t ESP_ERR_INVALID_STATE = 0x103;
constexpr esp_err_t ESP_ERR_NO_MEM = 0x101, ESP_ERR_INVALID_SIZE = 0x104;
constexpr const char* kTag = "problem-fixture";
void FixtureLog(const char*, const char*, ...) {}
#define ESP_LOGW(...) FixtureLog(__VA_ARGS__)
#define ESP_LOGI(...) FixtureLog(__VA_ARGS__)

namespace wqn {
enum class OutboxSuspendReason { kActorOwnership };
const char* OutboxSuspendReasonName(OutboxSuspendReason) { return "actor-ownership"; }
struct DurableProblemObservation {
    std::string request_id, problem_id, occurred_at;
    int action = 0;
};
struct ProblemOutboxSnapshot {
    size_t pending_count = 0, suspended_count = 0, capacity = 0;
};
constexpr size_t kProblemObservationOutboxCapacity = 200;
}
using Observation = wqn::DurableProblemObservation;
char kOutboxPath[512], kOutboxTempPath[512], kOutboxBackupPath[512], kParkedPath[512], kRejectedPath[512];
int read_opens = 0, write_opens = 0;
std::string bad_open_path, bad_read_path, bad_close_path, partial_write_path;
FILE* bad_close_stream = nullptr;
FILE* partial_write_stream = nullptr;
bool fail_publish=false, fail_restore=false, stop_after_rotate=false;
bool fail_empty_backup_cleanup=false, fail_newline=false;
struct PublicationStop {};
extern "C" FILE* __real_fopen(const char*, const char*);
extern "C" int __real_fclose(FILE*);
extern "C" size_t __real_fwrite(const void*, size_t, size_t, FILE*);
extern "C" int __real_rename(const char*, const char*);
extern "C" int __real_remove(const char*);
extern "C" int __real_fputc(int, FILE*);
extern "C" int __wrap_rename(const char* from, const char* to) {
    if ((fail_publish && std::strcmp(from,kOutboxTempPath)==0) ||
        (fail_restore && std::strcmp(from,kOutboxBackupPath)==0)) { errno=EIO; return -1; }
    const int result=__real_rename(from,to);
    if(result==0 && stop_after_rotate && std::strcmp(from,kOutboxPath)==0 &&
       std::strcmp(to,kOutboxBackupPath)==0) throw PublicationStop{};
    return result;
}
extern "C" int __wrap_remove(const char* path) {
    struct stat info={};
    if(fail_empty_backup_cleanup && std::strcmp(path,kOutboxBackupPath)==0 &&
       ::stat(kOutboxPath,&info)==0 && info.st_size==0) { errno=EIO; return -1; }
    return __real_remove(path);
}
extern "C" int __wrap_fputc(int ch, FILE* file) {
    return fail_newline && ch=='\n' ? EOF : __real_fputc(ch,file);
}
ssize_t FailRead(void*, char*, size_t) { errno = EIO; return -1; }
extern "C" FILE* __wrap_fopen(const char* path, const char* mode) {
    const bool reading = std::strcmp(mode, "rb") == 0;
    reading ? ++read_opens : ++write_opens;
    if (bad_open_path == path) { errno = EIO; return nullptr; }
    FILE* file;
    if (reading && bad_read_path == path) {
        cookie_io_functions_t functions = {};
        functions.read = FailRead;
        file = fopencookie(nullptr, "r", functions);
    } else {
        file = __real_fopen(path, mode);
    }
    if (file != nullptr && bad_close_path == path) bad_close_stream = file;
    if (file != nullptr && !reading && partial_write_path == path) partial_write_stream = file;
    return file;
}
extern "C" int __wrap_fclose(FILE* file) {
    const bool fail = file == bad_close_stream;
    if (fail) bad_close_stream = nullptr;
    if (file == partial_write_stream) partial_write_stream = nullptr;
    const int result = __real_fclose(file);
    if (fail) { errno = EIO; return EOF; }
    return result;
}
extern "C" size_t __wrap_fwrite(const void* data, size_t size, size_t count, FILE* file) {
    if (file == partial_write_stream && count > 0) {
        // Real partial bytes remain on disk after the failed close/append.
        return __real_fwrite(data, size, count / 2, file);
    }
    return __real_fwrite(data, size, count, file);
}

@@STATE@@
@@VALIDATION@@

std::string EncodeObservationLine(const Observation& value) {
    return value.request_id + '|' + value.problem_id + '|' + value.occurred_at;
}
enum class ObservationDecode { kValid, kMalformed, kUnavailable };
ObservationDecode DecodeObservationLine(const std::string& line, Observation* out) {
    const size_t a = line.find('|'), b = line.find('|', a + 1);
    if (a == std::string::npos || b == std::string::npos) return ObservationDecode::kMalformed;
    out->request_id = line.substr(0, a);
    out->problem_id = line.substr(a + 1, b - a - 1);
    out->occurred_at = line.substr(b + 1);
    return IsValidObservation(*out) ? ObservationDecode::kValid : ObservationDecode::kMalformed;
}

@@IO@@
@@TRANSACTIONS@@

int passes = 0, failures = 0;
void Check(bool condition, const char* name) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    condition ? ++passes : ++failures;
}
void Reset() {
    fail_publish=fail_restore=stop_after_rotate=fail_empty_backup_cleanup=fail_newline=false;
    bad_open_path.clear(); bad_read_path.clear(); bad_close_path.clear(); partial_write_path.clear();
    bad_close_stream = partial_write_stream = nullptr;
    for (const char* path : {kOutboxPath, kOutboxTempPath, kOutboxBackupPath, kParkedPath, kRejectedPath}) {
        if (std::remove(path) != 0 && errno != ENOENT) std::abort();
    }
    read_opens = write_opens = 0;
    @@RESET_CACHE@@
}
Observation Value(const char* suffix = "A") {
    return {std::string("fixture-request-") + suffix,
            "00000000-0000-0000-0000-000000000001", "2026-10-08T00:00:00Z"};
}
void RawPut(const std::string& bytes) {
    FILE* f=__real_fopen(kOutboxPath,"wb");
    if(!f || __real_fwrite(bytes.data(),1,bytes.size(),f)!=bytes.size() || __real_fclose(f)!=0) std::abort();
}
esp_err_t Commit(const Observation& value) {
    CommitContext context{&value}; return CommitTransaction(&context);
}
esp_err_t Remove(const Observation& value, bool quarantine = false) {
    RequestIdContext context{&value.request_id, quarantine}; return RemoveTransaction(&context);
}
esp_err_t Suspend(const Observation& value) {
    ParkContext context{&value.request_id, wqn::OutboxSuspendReason::kActorOwnership};
    return SuspendTransaction(&context);
}
esp_err_t Peek(Observation* out) { *out = {}; return PeekTransaction(out); }
esp_err_t Snapshot(wqn::ProblemOutboxSnapshot* out) { *out = {}; return SnapshotTransaction(out); }
bool PrimeEmpty() {
    Observation value;
    return Peek(&value) == ESP_ERR_NOT_FOUND;
}
void RecoveryCases() {
    const auto a=Value("A"), b=Value("B"); Observation out;
    Reset(); Check(Commit(a)==ESP_OK && Commit(b)==ESP_OK,"seed two accepted verdicts");
    fail_publish=fail_restore=true;
    Check(Remove(a)==ESP_FAIL,"failed publication and rollback report failure");
    fail_publish=fail_restore=false; g_empty_problem_outbox_known=false;
    Check(Peek(&out)==ESP_OK && out.request_id==a.request_id,"latest backup restores all accepted verdicts");
    Check(Remove(a)==ESP_OK && Peek(&out)==ESP_OK && out.request_id==b.request_id,
          "retry ACK preserves the next unuploaded verdict");
    Reset(); Commit(a); Commit(b); stop_after_rotate=true; bool stopped=false;
    try { Remove(a); } catch(PublicationStop&) { stopped=true; }
    stop_after_rotate=false; g_empty_problem_outbox_known=false;
    Check(stopped && Peek(&out)==ESP_OK && out.request_id==a.request_id,
          "restart after primary rotation recovers accepted records");
    Reset(); Commit(a); fail_empty_backup_cleanup=true;
    Check(Remove(a)==ESP_OK,"empty publication succeeds despite deferred backup cleanup");
    fail_empty_backup_cleanup=false; g_empty_problem_outbox_known=false;
    Check(Peek(&out)==ESP_ERR_NOT_FOUND,"committed empty primary never resurrects an ACKed backup");
    Check(Commit(b)==ESP_OK && Peek(&out)==ESP_OK && out.request_id==b.request_id,
          "new append starts from the authoritative empty generation");
    Reset(); partial_write_path=kOutboxPath;
    Check(Commit(a)==ESP_FAIL,"leave real partial bytes before retry");
    partial_write_path.clear();
    Check(Commit(a)==ESP_OK && Peek(&out)==ESP_OK && out.request_id==a.request_id,
          "matching text in partial bytes cannot report durable success");
    Reset(); partial_write_path=kOutboxPath; Commit(a); partial_write_path.clear();
    Check(Commit(b)==ESP_OK && Peek(&out)==ESP_OK && out.request_id==b.request_id,
          "successful next append repairs the preceding broken frame");
    Reset(); fail_newline=true;
    Check(Commit(a)==ESP_FAIL,"missing newline is a failed uncommitted frame");
    fail_newline=false;
    Check(Commit(a)==ESP_OK && Peek(&out)==ESP_OK && out.request_id==a.request_id,
          "retry of a full JSON body without newline writes a framed durable record");
    Reset(); const auto longer=Value("A-longer"); Commit(longer);
    Check(Commit(a)==ESP_OK && Remove(longer)==ESP_OK && Peek(&out)==ESP_OK && out.request_id==a.request_id,
          "request ID prefix does not suppress a distinct observation");
    Reset(); Commit(a); auto conflict=a; conflict.problem_id="00000000-0000-0000-0000-000000000002";
    Check(Commit(conflict)==ESP_ERR_INVALID_STATE && Peek(&out)==ESP_OK && out.problem_id==a.problem_id,
          "same request ID with changed payload is rejected");
    Reset(); RawPut(std::string(513,'x')+EncodeObservationLine(a)+'\n'+EncodeObservationLine(b)+'\n');
    Check(Peek(&out)==ESP_OK && out.request_id==b.request_id,
          "overlong line cannot expose its valid-looking suffix as a record");
#if REAL_CODEC
    Reset(); Commit(a); conflict=a;
    conflict.action=wqn::protocol::problem_study_v1::ReviewAction::kWrong;
    Check(Commit(conflict)==ESP_ERR_INVALID_STATE,"same ID cannot change the review action");
    conflict=a; conflict.occurred_at="2026-10-09T00:00:00Z";
    Check(Commit(conflict)==ESP_ERR_INVALID_STATE,"same ID cannot change the observation timestamp");
    Check(DecodeObservationLine(EncodeObservationLine(a)+"garbage",&out) == ObservationDecode::kMalformed,
          "production JSON decoder rejects trailing non-JSON bytes");
    Check(DecodeObservationLine(EncodeObservationLine(a)+std::string("\0junk",5),&out) == ObservationDecode::kMalformed,
          "production JSON decoder rejects embedded NUL suffixes");
    Reset(); RawPut(EncodeObservationLine(a)+std::string("\0junk\n",6)+EncodeObservationLine(b)+'\n');
    Check(Peek(&out)==ESP_OK && out.request_id==b.request_id,
          "NUL-corrupted line cannot hide a valid record behind it");
    Reset(); std::string padded=EncodeObservationLine(a); padded.pop_back(); padded+=",\"padding\":\"";
    padded.append(512-padded.size()-2,'x'); padded+="\"}";
    RawPut(padded+"\r\n");
    Check(Peek(&out)==ESP_OK && out.request_id==a.request_id,
          "maximum-sized valid JSON survives CRLF at a chunk boundary");
#endif
}
#if REAL_CODEC
static bool arm_encoder = false, encode_allocations = false;
static unsigned encoder_allocations = 0, encoder_live_nodes = 0, encoder_denials = 0;
static int fail_encoder_at = -1;
extern "C" cJSON* __real_cJSON_CreateObject(void);
extern "C" cJSON* __wrap_cJSON_CreateObject(void) {
    if (arm_encoder) {
        arm_encoder = false; encode_allocations = true; encoder_allocations = 0;
    }
    return __real_cJSON_CreateObject();
}
static void* EncoderAllocate(size_t bytes) {
    if (encode_allocations && encoder_allocations++ == static_cast<unsigned>(fail_encoder_at)) {
        ++encoder_denials;
        return nullptr;  // All later allocations recover.
    }
    void* pointer = std::malloc(bytes);
    if (pointer) ++encoder_live_nodes;
    return pointer;
}
static void EncoderFree(void* pointer) {
    if (pointer) {
        if (encoder_live_nodes == 0) std::abort();
        --encoder_live_nodes;
        std::free(pointer);
    }
}
static bool trace_decodes = false;
static int fail_decode_call = -1, fail_decode_site = -1;
static std::vector<std::string> watched_lines;
static std::vector<unsigned> decode_sites;
extern "C" cJSON* __real_cJSON_ParseWithOpts(const char*, const char**, cJSON_bool);
extern "C" cJSON* __wrap_cJSON_ParseWithOpts(const char* text, const char** end, cJSON_bool complete) {
    if (!trace_decodes || std::find(watched_lines.begin(), watched_lines.end(), text) == watched_lines.end()) {
        return __real_cJSON_ParseWithOpts(text, end, complete);
    }
    const bool previous_tracking = encode_allocations;
    const int previous_failure = fail_encoder_at;
    encode_allocations = true; encoder_allocations = 0;
    fail_encoder_at = static_cast<int>(decode_sites.size()) == fail_decode_call ? fail_decode_site : -1;
    cJSON* result = __real_cJSON_ParseWithOpts(text, end, complete);
    decode_sites.push_back(encoder_allocations);
    encode_allocations = previous_tracking; fail_encoder_at = previous_failure;
    return result;
}
static std::string FileBytes(const char* path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
static void RawPutAt(const char* path, const std::string& bytes) {
    FILE* file = __real_fopen(path, "wb");
    if (!file || __real_fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size() ||
        __real_fclose(file) != 0) std::abort();
}
static void EncoderAllocationCases() {
    cJSON_Hooks hooks{EncoderAllocate, EncoderFree}; cJSON_InitHooks(&hooks);
    auto previous = Value("previous"), observation = Value("encoder"), older = Value("older");
    for (unsigned action = 0; action < 4; ++action) {
        observation.action = static_cast<wqn::protocol::problem_study_v1::ReviewAction>(action);
        arm_encoder = true; encode_allocations = false; fail_encoder_at = -1;
        const std::string golden = EncodeObservationLine(observation);
        encode_allocations = false;
        const unsigned allocation_sites = encoder_allocations;
        Observation decoded;
        Check(!golden.empty() && DecodeObservationLine(golden, &decoded) == ObservationDecode::kValid &&
              decoded.request_id == observation.request_id && decoded.problem_id == observation.problem_id &&
              decoded.action == observation.action && decoded.occurred_at == observation.occurred_at,
              "all problem actions round trip through the real JSON codec");
        Check(encoder_live_nodes == 0 && allocation_sites > 0, "encoder frees its successful JSON tree");
        for (unsigned state = 0; state < 3; ++state) for (unsigned site = 0; site < allocation_sites; ++site) {
            Reset();
            if (state) {
                RawPut(EncodeObservationLine(previous) + '\n' + (state == 2 ? "{torn" : ""));
                RawPutAt(kOutboxBackupPath, EncodeObservationLine(older) + '\n');
                RawPutAt(kParkedPath, ""); RawPutAt(kRejectedPath, "");
            }
            const std::string original = FileBytes(kOutboxPath), backup = FileBytes(kOutboxBackupPath);
            write_opens = 0; encoder_denials = 0; arm_encoder = true;
            encode_allocations = false; fail_encoder_at = static_cast<int>(site);
            const int result = Commit(observation);
            encode_allocations = false; fail_encoder_at = -1;
            if (result == ESP_OK) {
                std::string accepted = FileBytes(kOutboxPath);
                if (!accepted.empty() && accepted.back() == '\n') accepted.pop_back();
                const size_t separator = accepted.rfind('\n');
                if (separator != std::string::npos) accepted.erase(0, separator + 1);
                Observation read_back;
                const bool decodable = DecodeObservationLine(accepted, &read_back) == ObservationDecode::kValid;
                std::fprintf(stderr, "FALSE ACK: action=%u state=%u allocation=%u wrote=%d decodable=%d\n",
                             action, state, site, write_opens, decodable ? 1 : 0);
            }
            Check(result != ESP_OK, "encoder allocation failure never acknowledges a partial observation");
            if (result == ESP_OK) return;
            Check(encoder_denials == 1 && encoder_live_nodes == 0,
                  "one failed encoder allocation recovers and releases every JSON node");
            if (encoder_live_nodes != 0) return;
            Check(write_opens == 0 && FileBytes(kOutboxPath) == original && FileBytes(kOutboxBackupPath) == backup,
                  "failed encoding preserves primary and backup before tail repair or append");
            Check(::access(kOutboxTempPath, F_OK) != 0 &&
                  FileBytes(kParkedPath).empty() && FileBytes(kRejectedPath).empty(),
                  "failed encoding leaves no temp file or new markers");
            if (state) Check(Peek(&decoded) == ESP_OK && decoded.request_id == previous.request_id,
                             "previous accepted observation remains queryable after failed encoding");
            Check(Commit(observation) == ESP_OK, "observation retry succeeds after encoder allocation recovers");
            const std::string committed = FileBytes(kOutboxPath);
            Check(committed.find(golden + '\n') != std::string::npos,
                  "retry persists the complete original observation");
            const int writes = write_opens;
            Check(Commit(observation) == ESP_OK && write_opens == writes && FileBytes(kOutboxPath) == committed,
                  "retry remains idempotent after successful persistence");
            Check(encoder_live_nodes == 0, "retry/query cycle releases all cJSON allocations");
        }
        std::printf("Encoder action=%u: %u allocation sites x 3 journal states PASS\n", action, allocation_sites);
    }
    Reset(); arm_encoder = true; encode_allocations = false; fail_encoder_at = 0;
    Check(Commit(observation) == ESP_ERR_NO_MEM, "empty encoder output reports memory failure");
    encode_allocations = false; fail_encoder_at = -1;
    Check(encoder_live_nodes == 0, "final failed root allocation leaves no JSON nodes");
    cJSON_InitHooks(nullptr);
}

static void DecoderAllocationCases() {
    cJSON_Hooks hooks{EncoderAllocate, EncoderFree}; cJSON_InitHooks(&hooks);
    const auto a = Value("decode-A"), b = Value("decode-B"), next = Value("decode-new");
    auto conflict = a; conflict.occurred_at = "2026-10-09T00:00:00Z";
    const std::string line_a = EncodeObservationLine(a), line_b = EncodeObservationLine(b);
    watched_lines = {line_a, line_b};
    auto prepare = [&]() {
        trace_decodes = false; Reset();
        Check(Commit(a) == ESP_OK && Commit(b) == ESP_OK, "decoder fixture records were durably accepted");
        // A complete bad frame and torn tail keep repair/rewrite in the path.
        RawPut(FileBytes(kOutboxPath) + "{bad}\n{torn");
        RawPutAt(kOutboxBackupPath, line_a + '\n');
        RawPutAt(kParkedPath, ""); RawPutAt(kRejectedPath, "");
        write_opens = 0; decode_sites.clear(); encoder_denials = 0;
    };
    auto operation = [&](unsigned action) {
        Observation out; wqn::ProblemOutboxSnapshot snapshot;
        switch (action) {
            case 0: return Commit(next);
            case 1: return Commit(a);
            case 2: return Commit(conflict);
            case 3: return Peek(&out);
            case 4: return Snapshot(&snapshot);
            case 5: return Remove(b);
            case 6: return Remove(b, true);
            default: return Suspend(b);
        }
    };
    unsigned faults = 0;
    for (unsigned action = 0; action < 8; ++action) {
        prepare(); trace_decodes = true; fail_decode_call = fail_decode_site = -1;
        Check(operation(action) == (action == 2 ? ESP_ERR_INVALID_STATE : ESP_OK),
              "normal decoder transaction preserves its result contract");
        trace_decodes = false;
        const auto sites = decode_sites;
        Check(!sites.empty() && encoder_live_nodes == 0, "decoder baseline records allocations and frees its tree");
        for (unsigned call = 0; call < sites.size(); ++call) for (unsigned site = 0; site < sites[call]; ++site) {
            prepare();
            const std::string primary = FileBytes(kOutboxPath), backup = FileBytes(kOutboxBackupPath);
            trace_decodes = true; fail_decode_call = static_cast<int>(call); fail_decode_site = static_cast<int>(site);
            const int result = operation(action);
            trace_decodes = false; fail_decode_call = fail_decode_site = -1;
            if (result != ESP_ERR_NO_MEM || encoder_denials != 1) {
                std::fprintf(stderr, "DECODER FAULT: action=%u call=%u site=%u result=%d denials=%u\n",
                             action, call, site, result, encoder_denials);
            }
            Check(result == ESP_ERR_NO_MEM && encoder_denials == 1,
                  "every valid-record decode allocation failure returns a retryable memory error");
            Check(encoder_live_nodes == 0, "failed decoder releases every partially built JSON node");
            Check(write_opens == 0 && FileBytes(kOutboxPath) == primary && FileBytes(kOutboxBackupPath) == backup &&
                  ::access(kOutboxTempPath, F_OK) != 0 && FileBytes(kParkedPath).empty() && FileBytes(kRejectedPath).empty(),
                  "failed decoding preserves primary, backup and marker journals without writes");
            // Resume another local save then ACK it: A/B must survive both rewrites.
            wqn::ProblemOutboxSnapshot snapshot;
            Check(Commit(next) == ESP_OK && Remove(next) == ESP_OK && Snapshot(&snapshot) == ESP_OK &&
                  snapshot.pending_count == 2 && FileBytes(kOutboxPath).find(line_a) != std::string::npos &&
                  FileBytes(kOutboxPath).find(line_b) != std::string::npos,
                  "accepted records survive recovered append and subsequent ACK");
            Check(operation(action) == (action == 2 ? ESP_ERR_INVALID_STATE : ESP_OK),
                  "failed decoder operation can be retried after memory recovers");
            Check(encoder_live_nodes == 0, "decoder retry releases every JSON allocation");
            ++faults;
        }
    }
    // Noncanonical but readable records must receive the same protection. Extra
    // fields exercise strings, escapes, nested containers, scalars and cJSON's
    // permissive legacy number/Unicode syntax without changing the wire format.
    std::vector<std::string> extras = {
        "null", "true", "false", "0", "-0", "001", "-.5", "1.", "1.e+02", "1e309",
        "[]", "{}", "[{},true,false,null,1.5e-2]", "{\"nested\":[1,{\"x\":2}]}",
        R"("\"\\\/\b\f\n\r\t")", R"("\u0000\u00ff\uD834\uDD1E")", R"("\uxxxx")", R"("\u1\"f")",
        std::string("\"raw") + '\x01' + "\"",
        std::string(31, '[') + "0" + std::string(31, ']'),
    };
    unsigned compatibility_faults = 0;
    for (const auto& extra : extras) {
        const std::string line = "\xef\xbb\xbf \t" + line_a.substr(0, line_a.size()-1) + ",\"extra\":" + extra + "}\r ";
        Observation out;
        encode_allocations = true; encoder_allocations = 0; fail_encoder_at = -1;
        const auto decoded = DecodeObservationLine(line, &out);
        encode_allocations = false;
        const unsigned sites = encoder_allocations;
        Check(decoded == ObservationDecode::kValid && out.request_id == a.request_id,
              "legacy JSON syntax remains readable with extra fields and BOM/whitespace");
        for (unsigned site = 0; site < sites; ++site) {
            encode_allocations = true; encoder_allocations = 0; fail_encoder_at = static_cast<int>(site); encoder_denials = 0;
            const auto result = DecodeObservationLine(line, &out);
            encode_allocations = false; fail_encoder_at = -1;
            Check(result == ObservationDecode::kUnavailable && encoder_denials == 1 && encoder_live_nodes == 0,
                  "allocation failure cannot turn a readable legacy JSON record into malformed data");
            ++compatibility_faults;
        }
    }
    const std::vector<std::string> malformed = {
        "", "garbage", "{bad}", "[", "{\"x\":}", "[1,]", "{\"x\":1,}", "{\"x\" 1}",
        "{\"x\":true false}", "{\"x\":1e}", "{\"x\":-.}", "{\"x\":[1,2}",
        R"({"x":"unterminated})", R"({"x":"\q"})", R"({"x":"\u12"})",
        R"({"x":"\ud800"})", R"({"x":"\udc00"})", R"({"x":"\ud800\u0000"})",
        line_a + "garbage", line_a + line_b,
    };
    for (const auto& line : malformed) {
        Observation out;
        Check(DecodeObservationLine(line, &out) == ObservationDecode::kMalformed,
              "syntax-corrupt complete frames still follow the bounded repair policy");
        encode_allocations = true; encoder_allocations = 0; fail_encoder_at = 0;
        const auto result = DecodeObservationLine(line, &out);
        encode_allocations = false; fail_encoder_at = -1;
        Check(result == ObservationDecode::kMalformed && encoder_live_nodes == 0,
              "independently proven corrupt syntax stays recoverable even during memory pressure");
    }
    // Compare the probe against real cJSON on deterministic one-byte mutations.
    // Only accepted syntax is authoritative here: a conservative false-positive
    // delays repair, whereas a false-negative could delete a valid record.
    const std::string seed = "{\"a\":[true,false,null,-.12e+3,{\"x\":\"\\uD834\\uDD1E\"}]}";
    std::string replacements;
    for (unsigned byte = 1; byte < 128; ++byte) replacements.push_back(static_cast<char>(byte));
    unsigned accepted = 0;
    bool all_safe = true;
    for (size_t index = 0; index < seed.size(); ++index) for (char ch : replacements) {
        std::string changed = seed; changed[index] = ch;
        cJSON* root = cJSON_ParseWithOpts(changed.c_str(), nullptr, true);
        if (root) { ++accepted; all_safe = all_safe && ObservationJsonSyntax(changed).Complete(); cJSON_Delete(root); }
    }
    Check(accepted > 50 && all_safe && encoder_live_nodes == 0,
          "syntax probe never rejects cJSON-accepted one-byte mutation corpus");
    std::printf("Decoder: %u transaction faults, %u compatibility faults, %u accepted syntax mutations PASS\n",
                faults, compatibility_faults, accepted);
    Reset(); watched_lines.clear(); cJSON_InitHooks(nullptr);
}
#endif
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 2) return 2;
    std::snprintf(kOutboxPath, sizeof(kOutboxPath), "%s/outbox", argv[1]);
    std::snprintf(kOutboxTempPath, sizeof(kOutboxTempPath), "%s/temp", argv[1]);
    std::snprintf(kOutboxBackupPath, sizeof(kOutboxBackupPath), "%s/outbox.bak", argv[1]);
    std::snprintf(kParkedPath, sizeof(kParkedPath), "%s/parked", argv[1]);
    std::snprintf(kRejectedPath, sizeof(kRejectedPath), "%s/rejected", argv[1]);
    Observation out, a = Value(), b = Value("B");
    wqn::ProblemOutboxSnapshot snapshot;
    Reset();
    Check(PrimeEmpty() && read_opens == 2, "cold query validates both absent journals");
    int previous_reads = read_opens;
    Check(Peek(&out) == ESP_ERR_NOT_FOUND && read_opens == previous_reads,
          "repeated empty peek avoids file opens");
    Check(Snapshot(&snapshot) == ESP_OK && snapshot.pending_count == 0 &&
          snapshot.suspended_count == 0 && snapshot.capacity == 200 && read_opens == previous_reads,
          "empty snapshot reuses the same evidence without file opens");
    Reset();
    Check(Snapshot(&snapshot) == ESP_OK && read_opens == 2, "snapshot can establish empty evidence");
    previous_reads = read_opens;
    Check(Peek(&out) == ESP_ERR_NOT_FOUND && read_opens == previous_reads,
          "peek shares evidence established by a snapshot");
    Check(Commit(a) == ESP_OK && Peek(&out) == ESP_OK && out.request_id == a.request_id,
          "new durable append invalidates empty evidence before it can hide the observation");
    Check(Commit(a) == ESP_OK && Snapshot(&snapshot) == ESP_OK && snapshot.pending_count == 1,
          "idempotent retry does not create another pending record");
    Check(Suspend(a) == ESP_OK && Peek(&out) == ESP_ERR_NOT_FOUND &&
          Snapshot(&snapshot) == ESP_OK && snapshot.suspended_count == 1,
          "parked payload is not mistaken for a physically empty outbox");
    previous_reads = read_opens;
    Check(Peek(&out) == ESP_ERR_NOT_FOUND && read_opens > previous_reads,
          "all-parked queue remains uncached and preserves marker checks");
    Check(Commit(b) == ESP_OK && Peek(&out) == ESP_OK && out.request_id == b.request_id,
          "new observation behind a parked payload is still visible");
    Check(Remove(b) == ESP_OK && Snapshot(&snapshot) == ESP_OK && snapshot.suspended_count == 1,
          "ACK of active record retains the parked payload");
    Check(Remove(a) == ESP_OK && PrimeEmpty(), "empty rewrite is revalidated after ACK");
    previous_reads = read_opens;
    Check(Peek(&out) == ESP_ERR_NOT_FOUND && read_opens == previous_reads,
          "orphan park marker does not prevent reuse of a validated empty payload file");
    Reset(); PrimeEmpty(); bad_open_path = kOutboxPath;
    Check(Commit(a) == ESP_FAIL, "failed append open is reported");
    Check(Peek(&out) == ESP_FAIL, "failed append attempt cannot retain stale empty evidence");
    bad_open_path.clear();
    Check(PrimeEmpty(), "query recovers after the append-open error clears");
    Reset(); PrimeEmpty(); partial_write_path = kOutboxPath;
    Check(Commit(a) == ESP_FAIL, "partial append is reported as failure");
    partial_write_path.clear(); previous_reads = read_opens;
    Check(Peek(&out) == ESP_ERR_NOT_FOUND && read_opens > previous_reads,
          "partial append forces a physical reread rather than cached empty success");
    previous_reads = read_opens;
    Check(Peek(&out) == ESP_ERR_NOT_FOUND && read_opens > previous_reads,
          "malformed nonempty payload never establishes empty evidence");
    Reset();
    Check(WriteOutboxLines({""}) == ESP_OK && PrimeEmpty(), "blank-only payload has no uploadable record");
    previous_reads = read_opens;
    Check(Peek(&out) == ESP_ERR_NOT_FOUND && read_opens > previous_reads,
          "discarded blank bytes do not count as a physically empty file");
    Reset();
    Check(WriteOutboxLines({std::string(kMaxLineBytes + 1, 'x')}) == ESP_OK && PrimeEmpty(),
          "overlong-only payload follows the existing malformed-line policy");
    previous_reads = read_opens;
    Check(Peek(&out) == ESP_ERR_NOT_FOUND && read_opens > previous_reads,
          "discarded overlong bytes cannot establish physically empty evidence");
    Reset(); PrimeEmpty(); bad_open_path = kOutboxPath;
    Check(WriteOutboxLines({EncodeObservationLine(a)}) == ESP_OK,
          "direct rewrite invalidates empty evidence independently of public transaction");
    Check(Peek(&out) == ESP_FAIL, "read failure after rewrite cannot be masked by old empty cache");
    bad_open_path.clear();
    Check(Peek(&out) == ESP_OK && out.request_id == a.request_id, "rewritten payload remains readable");
    Reset(); bad_open_path = kParkedPath;
    Check(Peek(&out) == ESP_FAIL, "park journal open error is not an empty queue");
    bad_open_path.clear(); previous_reads = read_opens;
    Check(PrimeEmpty() && read_opens > previous_reads, "failed cold query does not establish empty cache");
    for (const char* path : {kOutboxPath, kParkedPath}) {
        Reset(); bad_read_path = path;
        Check(Peek(&out) == ESP_FAIL, "real libc read EIO cannot become cached EOF");
        bad_read_path.clear(); previous_reads = read_opens;
        Check(PrimeEmpty() && read_opens > previous_reads, "read-EIO retry revalidates both journals");
        Reset(); bad_close_path = path;
        // The file must exist to reach fclose rather than ENOENT.
        FILE* empty = __real_fopen(path, "wb");
        if (empty == nullptr || __real_fclose(empty) != 0) return 2;
        Check(Peek(&out) == ESP_FAIL, "failed read close cannot establish empty cache");
        bad_close_path.clear(); previous_reads = read_opens;
        Check(PrimeEmpty() && read_opens > previous_reads, "read-close retry revalidates both journals");
    }
    Reset(); PrimeEmpty(); bad_close_path = kOutboxPath;
    Check(Commit(a) == ESP_FAIL, "append close failure is reported even when bytes landed");
    bad_close_path.clear();
    Check(Peek(&out) == ESP_OK && out.request_id == a.request_id,
          "bytes landed on failed append close are not hidden by stale cache");
    Reset(); PrimeEmpty(); bad_open_path = kParkedPath;
    // Successful payload rewrite and failed marker append are separate failures.
    Check(WriteOutboxLines({EncodeObservationLine(a)}) == ESP_OK && Suspend(a) == ESP_FAIL,
          "failed park attempt preserves the payload and reports error");
    bad_open_path.clear();
    Check(Peek(&out) == ESP_OK && out.request_id == a.request_id,
          "failed park does not hide the still-uploadable payload");
    Reset(); PrimeEmpty(); bad_open_path = kOutboxPath;
    Check(Remove(a) == ESP_FAIL && Peek(&out) == ESP_FAIL,
          "failed ACK preparatory read invalidates older empty evidence");
    Reset(); PrimeEmpty(); bad_open_path = kParkedPath;
    Check(Suspend(a) == ESP_FAIL && Peek(&out) == ESP_FAIL,
          "failed park preparatory read invalidates older empty evidence");
    RecoveryCases();
#if REAL_CODEC
    EncoderAllocationCases();
    DecoderAllocationCases();
#endif
    std::printf("%d PASS / %d FAIL (temporary libc files; NOT ESP32 HIL)\n",
                passes, failures);
    Reset();
    return failures ? 1 : 0;
}
