#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <vector>
#include "word_pack.h"
#include "note_pack.h"
#include "problem_pack.h"
#include "cJSON.h"
#include "device_protocol/json_depth_guard.h"
#if __has_include("pack_jsonl_reader.h")
#include "pack_jsonl_reader.h"
#endif

std::string pack_path;
int64_t esp_timer_get_time() { return 0; }
void MaybeYieldPackRebuild(int64_t&) {}
struct PackReadBuffer { explicit PackReadBuffer(FILE*) {} };
namespace wqn {
std::string SafePackStem(const WqnWordPackManifestItem&) { return "synthetic"; }
std::string SafeNotePackStem(const WqnNotePackManifestNotebook&) { return "synthetic"; }
std::string SafeProblemPackStem(const WqnProblemPackManifestSet&) { return "synthetic"; }
}
@@PRODUCTION@@

unsigned checks=0,failures=0;
void Check(bool okay,const std::string& label)
{
    ++checks;
    if(!okay) { ++failures; if(failures<30) std::fprintf(stderr,"FAIL %s\n",label.c_str()); }
}
void Write(const std::string& bytes)
{
    FILE* file=std::fopen(pack_path.c_str(),"wb");
    if(!file || std::fwrite(bytes.data(),1,bytes.size(),file)!=bytes.size() || std::fclose(file)) std::abort();
}
std::string Read()
{
    FILE* file=std::fopen(pack_path.c_str(),"rb"); std::string bytes; char buffer[2048];size_t n;
    if(!file) std::abort();
    while((n=std::fread(buffer,1,sizeof(buffer),file))>0) bytes.append(buffer,n);
    if(std::ferror(file) || std::fclose(file)) std::abort();
    return bytes;
}
const std::string id="11111111-1111-4111-8111-111111111111";
std::string Row(const std::string& domain)
{
    if(domain=="word") return "{\"id\":\""+id+"\",\"deck_id\":\""+id+"\",\"word\":\"test\",\"normalized_word\":\"test\",\"revision\":1,\"sort_index\":0}";
    if(domain=="note") return "{\"note_id\":\""+id+"\",\"notebook_id\":\""+id+"\",\"title\":\"标题\",\"content\":\"正文\",\"revision\":1,\"sort_index\":0}";
    return "{\"problem_id\":\""+id+"\",\"title\":\"题目\",\"status\":\"wrong\",\"is_optional\":false,\"image_ids\":[],\"solution_image_ids\":[],\"parts\":[{\"index\":1,\"label\":\"A\",\"type\":\"short_answer\",\"full_marks\":1,\"content_text\":\"text\",\"answer_text\":\"answer\"}],\"content_text\":\"body\"}";
}
std::string Metadata(const std::string& domain,size_t count)
{
    if(domain=="word") return "{\"deck_id\":\""+id+"\",\"revision\":1,\"schema_version\":"+std::to_string(wqn::protocol::word_study_v1::kPackSchemaVersion)+",\"format\":\"jsonl\",\"compression\":\"none\",\"lexicon_type\":\"english_word\",\"entry_count\":"+std::to_string(count)+"}";
    const auto schema=domain=="note"?note_fixture::kPackSchemaVersion:problem_fixture::kPackSchemaVersion;
    const std::string key=domain=="note"?"notebook_id":"problem_set_id";
    const std::string revision=domain=="note"?"content_revision":"pack_revision";
    return "{\"v\":"+std::to_string(schema)+",\""+key+"\":\""+id+"\",\""+revision+"\":1,\"count\":"+std::to_string(count)+"}";
}
std::string Body(const std::string& domain,const std::string& metadata,const std::string& rows)
{ return (domain=="word"?std::string(word_fixture::kPackMagic)+"\n":"")+metadata+"\n"+rows; }
int Scan(const std::string& domain,size_t count,size_t* size,std::string* identity)
{
    if(domain=="word") {
        wqn::WqnWordPackManifestItem item;item.deck_id=id;item.content_revision=1;item.entry_count=count;
        wqn::WordPackIndex index;index.entries.resize(1);std::strcpy(index.entries[0].word_id,"sentinel");
        const auto result=word_fixture::ScanPackFile(item,0,&index);*size=index.entries.size();
        *identity=index.entries.size()>1?index.entries[1].word_id:index.entries[0].word_id;return result;
    }
    if(domain=="note") {
        wqn::WqnNotePackManifestNotebook item;item.notebook_id=id;item.content_revision=1;item.entry_count=count;
        wqn::NotePackIndex index;index.entries.resize(1);std::strcpy(index.entries[0].note_id,"sentinel");
        const auto result=note_fixture::ScanNotePackFile(item,0,&index,nullptr);*size=index.entries.size();
        *identity=index.entries.size()>1?index.entries[1].note_id:index.entries[0].note_id;return result;
    }
    wqn::WqnProblemPackManifestSet item;item.problem_set_id=id;item.pack_revision=1;item.entry_count=count;
    wqn::ProblemPackIndex index;index.entries.resize(1);std::strcpy(index.entries[0].problem_id,"sentinel");
    const auto result=problem_fixture::ScanProblemPackFile(item,0,&index,nullptr);*size=index.entries.size();
    *identity=index.entries.size()>1?index.entries[1].problem_id:index.entries[0].problem_id;return result;
}
std::string Replace(std::string text,const std::string& old,const std::string& value)
{ const auto pos=text.find(old);if(pos==std::string::npos) std::abort();text.replace(pos,old.size(),value);return text; }
void Expect(const std::string& domain,const std::string& name,const std::string& bytes,size_t count,bool valid)
{
    Write(bytes);size_t size=0;std::string identity;
    const auto result=Scan(domain,count,&size,&identity);
    Check((result==ESP_OK)==valid,domain+"/"+name+" scan result");
    Check(valid?(size==count+1 && identity==id):(size==1 && identity=="sentinel"),domain+"/"+name+" transactional index identity");
    Check(Read()==bytes,domain+"/"+name+" original file bytes retained");
}
void ReaderCases(const std::string& domain)
{
    using Reader=esp_err_t(*)(FILE*,std::vector<char>*,std::string*);
    Reader reader=domain=="word"?word_fixture::ReadBoundedPackLine:
                  domain=="note"?note_fixture::ReadBoundedNotePackLine:problem_fixture::ReadBoundedProblemPackLine;
    const auto capacity=domain=="word"?word_fixture::kLineBufferSize:
                        domain=="note"?note_fixture::kLineBufferSize:problem_fixture::kLineBufferSize;
    const auto maximum=domain=="word"?word_fixture::kMaxLineBytes:
                       domain=="note"?note_fixture::kMaxLineBytes:problem_fixture::kMaxLineBytes;
    std::vector<char> buffer(capacity,0);
    const auto test=[&](const std::string& name,const std::string& bytes,int expected,const std::string& text) {
        Write(bytes);FILE* file=std::fopen(pack_path.c_str(),"rb");if(!file) std::abort();
        std::string line="stale";const auto result=reader(file,&buffer,&line);
        Check(result==expected,domain+"/reader/"+name+" status");
        Check(expected==ESP_OK?line==text:line.empty(),domain+"/reader/"+name+" exact text/no partial text");
        std::fclose(file);Check(Read()==bytes,domain+"/reader/"+name+" retained bytes");
    };
    test("empty","",ESP_ERR_NOT_FOUND,"");
    test("max-LF",std::string(maximum,'x')+'\n',ESP_OK,std::string(maximum,'x'));
    test("max-CRLF",std::string(maximum,'x')+"\r\n",ESP_OK,std::string(maximum,'x'));
    test("over-max",std::string(maximum+1,'x')+'\n',ESP_ERR_INVALID_SIZE,"");
    test("max-EOF",std::string(maximum,'x'),domain=="word"?ESP_ERR_INVALID_SIZE:ESP_OK,std::string(maximum,'x'));
    for(size_t offset:{size_t(0),size_t(127),maximum-1}) {
        const auto prefix=std::string(offset,'x');
        test("raw-NUL-"+std::to_string(offset),prefix+'\0'+"tail\n",ESP_ERR_INVALID_RESPONSE,"");
    }
    Write("first\nsecond\r\n");FILE* file=std::fopen(pack_path.c_str(),"rb");if(!file) std::abort();
    std::string line;
    Check(reader(file,&buffer,&line)==ESP_OK && line=="first" && std::ftell(file)==6,
          domain+"/reader/first offset preserved");
    Check(reader(file,&buffer,&line)==ESP_OK && line=="second" && std::ftell(file)==14,
          domain+"/reader/reused buffer and CRLF offset preserved");
    Check(reader(file,&buffer,&line)==ESP_ERR_NOT_FOUND && line.empty(),domain+"/reader/reused EOF clears text");
    std::fclose(file);
}
int main(int argc,char** argv)
{
    if(argc!=2) return 2;
    pack_path=argv[1];
    for(const std::string domain:{"word","note","problem"}) {
        ReaderCases(domain);
        const auto row=Row(domain),metadata=Metadata(domain,1);
        const auto key=domain=="word"?"id":domain=="note"?"note_id":"problem_id";
        Expect(domain,"canonical",Body(domain,metadata,row+'\n'),1,true);
        Expect(domain,"EOF-terminated",Body(domain,metadata,row),1,domain!="word");
        Expect(domain,"CRLF",Body(domain,metadata+'\r',row+"\r\n"),1,true);
        const auto bad=Replace(row,id,id+R"(\u0000shadow)");
        Expect(domain,"value-NUL",Body(domain,metadata,bad+'\n'),1,false);
        Expect(domain,"key-NUL",Body(domain,metadata,Replace(row,"\""+std::string(key)+"\"","\""+std::string(key)+R"(\u0000shadow")")+'\n'),1,false);
        Expect(domain,"unknown-NUL",Body(domain,metadata,Replace(row,"{",R"({"extra":"x\u0000y",)")+'\n'),1,false);
        Expect(domain,"literal",Body(domain,metadata,Replace(row,"{",R"({"extra":"x\\u0000y",)")+'\n'),1,true);
        Expect(domain,"metadata-NUL",Body(domain,Replace(metadata,id,id+R"(\u0000shadow)"),row+'\n'),1,false);
        Expect(domain,"raw-NUL-EOF",Body(domain,metadata,row+'\0'+"ignored"),1,false);
        Expect(domain,"raw-NUL-LF",Body(domain,metadata,row+'\0'+"ignored\n"),1,false);
        Expect(domain,"late-bad-row",Body(domain,Metadata(domain,2),row+'\n'+bad+'\n'),2,false);
        Expect(domain,"Unicode-controls",Body(domain,metadata,Replace(row,"{",R"({"extra":"中文😀\n\t\u0001",)")+'\n'),1,true);
    }
    std::printf("Pack JSON identity: %u checks, %u failures; actual readers/row parsers/index scans, libc files; NOT HIL\n",checks,failures);
    return failures?1:0;
}
