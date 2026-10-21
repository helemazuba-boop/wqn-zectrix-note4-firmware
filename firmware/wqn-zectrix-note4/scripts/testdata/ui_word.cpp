#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include "font_fmt.h"
#include "font_zectrix.h"
#include "ui_fonts.h"
#include "word_fonts.h"

using esp_err_t=int;
constexpr int ESP_OK=0, ESP_FAIL=1;
int checks=0;
#define CHECK(c) do { if(!(c)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c);std::abort();}++checks; } while(0)
std::array<uint8_t,400*300> pixels;
struct Run { int x,y; const lv_font_t* font; std::string text; };
std::vector<Run> drawn;
int outside=0, refreshes=0;

namespace wqn {
constexpr int kEpdWidth=400,kEpdHeight=300;
enum class UiScreen { kWord, kNote };
namespace protocol::word_study_v1 {
enum class Mode { kSequential,kRandom,kDictionary,kReview,kIntake,kShuffle,kMistakes };
}
@@ENUMS@@
@@SNAPSHOT_TYPE@@
struct PersistedWordSession {
    bool active=false; uint32_t position=0,start_index=0;
    struct {std::vector<int> items; std::string session_id; protocol::word_study_v1::Mode mode=protocol::word_study_v1::Mode::kReview;} remote;
};
struct WqnWordEntry {std::string word,phonetic,meaning,example,example_translation,part_of_speech;};
struct WordAppState {
    WordAppMode mode=WordAppMode::kHome;
    WordCardPhase card_phase=WordCardPhase::kFront;
    WordHomeSelection home_selection=WordHomeSelection::kReview;
    bool cloud_sync_failed=false,review_session_resumable=false,shuffle_session_resumable=false,mistakes_session_resumable=false;
    uint16_t reviewed_today=20,correct_today=15;
    struct {WordObservationCommitState commit_state=WordObservationCommitState::kIdle; int batch_error=ESP_OK; bool batch_flush_requested=false,buffered=false; protocol::word_study_v1::Mode requested_mode=protocol::word_study_v1::Mode::kReview; PersistedWordSession persisted;} session;
    struct {bool mounted=true,truncated=false;std::vector<int> entries;size_t pack_count=1,pack_bytes=100;std::string status_message;} pack_index;
    struct {int cards_seen=0,reviewed=0,unknown=0,replayed=0;WordCompleteSelection complete_selection=WordCompleteSelection::kSequential;} review;
    struct {uint32_t sequential_cursor=128;} chain;
    struct {size_t pending_count=0,blocked_count=0,suspended_count=0;} outbox;
    WqnWordEntry current_word;
    std::string message,scoped_deck_title,default_deck_title;
};
bool HasPackWords(const WordAppState& state){return !state.pack_index.entries.empty();}
bool HasBufferedWordObservations(const WordAppState& state){return state.session.buffered;}
bool IsSequentialSession(const PersistedWordSession& s){return s.remote.mode==protocol::word_study_v1::Mode::kSequential;}
uint16_t ClampUint16(size_t v){return static_cast<uint16_t>(std::min<size_t>(v,UINT16_MAX));}
std::atomic<int32_t> g_word_review_due_count{-1},g_word_mistake_count{-1};
std::string WordAppProgressLabel(const WordAppState&);
std::string WordAppStatusLine(const WordAppState&);
@@COUNT_SETTERS@@
@@COUNT_GUARD@@
@@SNAPSHOT@@
struct HomeSummary {bool wifi_connected=true,charging=false,full=false;int wifi_rssi=-50,battery_percent=84;};
struct UiFrame {UiScreen screen=UiScreen::kWord;WordAppSnapshot word_app;HomeSummary home;};
void DrawEpdPixel(int x,int y,bool ink){if(x<0||x>=400||y<0||y>=300){if(ink)++outside;return;}pixels[y*400+x]=ink;}
void ClearEpdFramebuffer(bool white){pixels.fill(!white);drawn.clear();outside=0;}
extern "C" const lv_font_t SourceHanSansSC_Regular_slim;
extern "C" bool lv_font_get_glyph_dsc_fmt_txt(const lv_font_t*,lv_font_glyph_dsc_t*,uint32_t,uint32_t){return false;}
extern "C" const uint8_t* lv_font_get_bitmap_fmt_txt(const lv_font_t*,uint32_t){return nullptr;}
@@FONT_LOOKUP@@
@@FONT_DRAW@@
@@FONT_TEXT@@
void DrawTextWithFont(int x,int y,const lv_font_t* font,const char* text,bool ink){drawn.push_back({x,y,font,text});ProductionDrawTextWithFont(x,y,font,text,ink);}
const lv_font_t* PrimaryUiFont(){return &SourceHanSansSC_Regular_slim;}
}
namespace device_ui_internal {
@@LAYOUT@@
enum class RefreshSchedule{kSelection,kConfig,kCommit};
struct UiRect{int x,y,width,height;const char* name;};
void FillRect(int x,int y,int width,int height,bool ink){for(int row=y;row<y+height;++row)for(int col=x;col<x+width;++col)wqn::DrawEpdPixel(col,row,ink);}
void DrawHorizontalLine(int x,int y,int width){FillRect(x,y,width,1,true);}
int RefreshRegion(const UiRect&,RefreshSchedule){++refreshes;return ESP_OK;}
int RefreshStableRegion(const UiRect&,RefreshSchedule){++refreshes;return ESP_OK;}
int RefreshFrame(const wqn::UiFrame&,RefreshSchedule){++refreshes;return ESP_OK;}
@@BATTERY@@
@@ICONS@@
std::string Signature(const wqn::UiFrame& frame){std::string signature;
@@SIGNATURE@@
return signature;}
}
@@PAGE@@

std::string Text(){std::string result;for(const auto& run:drawn)result+=run.text;return result;}
bool Seen(const std::string& text){return Text().find(text)!=std::string::npos;}
void Export(const std::string& directory,const std::string& name){
    if(directory.empty())return;
    std::ofstream file(directory+"/"+name+".pbm",std::ios::binary);file<<"P4\n400 300\n";
    for(int y=0;y<300;++y)for(int x=0;x<400;x+=8){uint8_t byte=0;for(int i=0;i<8;++i)byte|=pixels[y*400+x+i]<<(7-i);file.put(static_cast<char>(byte));}
}
void Render(wqn::UiFrame frame,const std::string& directory,const std::string& name){
    const int before=refreshes;
    CHECK(device_ui_internal::RenderWordToEpd(frame,device_ui_internal::RefreshSchedule::kSelection)==ESP_OK);
    CHECK(refreshes==before+1);CHECK(outside==0);
    CHECK(!Seen("上下选择")&&!Seen("长按确认")&&!Seen("全文")&&!Seen("完整阅读"));
    CHECK(std::none_of(pixels.begin()+294*400,pixels.end(),[](uint8_t ink){return ink!=0;}));
    Export(directory,name);
}

int main(int argc,char** argv){
    using namespace wqn;using namespace device_ui_internal;
    const std::string output=argc>1?argv[1]:"";
    WordAppState state;state.pack_index.entries.resize(3200);state.default_deck_title="高中词汇";
    auto snapshot=BuildWordAppSnapshot(state);
    CHECK(!snapshot.review_due_known&&snapshot.review_due_count==0&&snapshot.mistake_count==-1);
    {WordDueCountFixtureGuard guard;SetWordReviewDueCount(0);}
    CHECK(!BuildWordAppSnapshot(state).review_due_known);
    SetWordReviewDueCount(24);{WordDueCountFixtureGuard guard;SetWordReviewDueCount(0);}
    CHECK(BuildWordAppSnapshot(state).review_due_count==24);
    SetWordReviewDueCount(0);SetWordMistakeCount(0);snapshot=BuildWordAppSnapshot(state);
    CHECK(snapshot.review_due_known&&snapshot.review_due_count==0&&snapshot.mistake_count==0);
    SetWordReviewDueCount(-1);CHECK(!BuildWordAppSnapshot(state).review_due_known);
    state.scoped_deck_title="专题词库";CHECK(BuildWordAppSnapshot(state).scope_title=="专题词库");state.scoped_deck_title.clear();
    state.session.batch_error=ESP_FAIL;state.message="暂存未保存，确认重试";snapshot=BuildWordAppSnapshot(state);
    CHECK(snapshot.save_failed&&!snapshot.saving&&snapshot.feedback==state.message);
    state.session.batch_error=ESP_OK;state.session.buffered=true;state.session.batch_flush_requested=true;state.message="正在保存暂存";
    snapshot=BuildWordAppSnapshot(state);CHECK(snapshot.saving&&!snapshot.save_failed&&snapshot.feedback==state.message);
    state.session.buffered=false;state.session.batch_flush_requested=false;state.message.clear();
    CHECK(BuildWordAppSnapshot(state).hint.empty());
    for(auto mode:{protocol::word_study_v1::Mode::kReview,protocol::word_study_v1::Mode::kIntake,protocol::word_study_v1::Mode::kSequential,protocol::word_study_v1::Mode::kShuffle,protocol::word_study_v1::Mode::kMistakes}){
        state.session.persisted.remote.mode=mode;CHECK(!BuildWordAppSnapshot(state).mode_label.empty());
    }
    state.session.persisted.remote.mode=protocol::word_study_v1::Mode::kReview;
    SetWordReviewDueCount(24);SetWordMistakeCount(18);
    UiFrame frame;frame.word_app=BuildWordAppSnapshot(state);Render(frame,output,"home");
    CHECK(Seen("今日学习")&&Seen("已复习 20")&&Seen("答对 15")&&Seen("24 到期")&&Seen("18 词"));
    for(int i=0;i<3;++i){frame.word_app.home_selection=static_cast<WordHomeSelection>(i);Render(frame,output,"focus-"+std::to_string(i));CHECK(pixels[(kWordMenuTop+i*kWordMenuPitch+10)*400+7]);}
    frame.word_app.review_due_known=false;frame.word_app.mistake_count=-1;Render(frame,output,"unknown");CHECK(Seen("-- 到期")&&Seen("-- 词"));
    frame.word_app.review_due_known=true;frame.word_app.review_due_count=0;frame.word_app.mistake_count=0;Render(frame,output,"zero");CHECK(Seen("0 到期")&&Seen("0 词"));
    frame.word_app.pack_ready=false;Render(frame,output,"unsynced");CHECK(Seen("未同步")&&Seen("需同步词库"));
    frame.word_app.pack_ready=true;frame.word_app.hint="本轮准备失败，可重试";Render(frame,output,"start-failed");CHECK(Seen(frame.word_app.hint));
    frame.word_app.hint.clear();frame.word_app.mode=WordAppMode::kWordCard;frame.word_app.has_card=true;frame.word_app.word="abandon";frame.word_app.phonetic="/əˈbændən/";
    frame.word_app.meaning="放弃；抛弃";frame.word_app.part_of_speech="v.";frame.word_app.example="He would never abandon his friends.";frame.word_app.example_translation="他绝不会抛弃自己的朋友。";frame.word_app.progress_line="1/3";
    Render(frame,output,"front");CHECK(Seen("abandon")&&Seen("/əˈbændən/")&&Seen("回忆这个词的意思"));CHECK(!Seen("放弃"));
    auto first=std::find_if(drawn.begin(),drawn.end(),[](const Run& r){return r.font==&wqn_word_bold_32;});CHECK(first!=drawn.end());const int front_y=first->y;
    frame.word_app.card_phase=WordCardPhase::kRevealed;Render(frame,output,"back");CHECK(Seen("放弃；抛弃")&&Seen(frame.word_app.example)&&Seen(frame.word_app.example_translation));
    first=std::find_if(drawn.begin(),drawn.end(),[](const Run& r){return r.font==&wqn_word_bold_32;});CHECK(first!=drawn.end()&&first->y==front_y);
    const auto original_card=frame.word_app;
    // The preset vocabulary exports both a POS field and a POS-prefixed meaning.
    frame.word_app.word="survey";frame.word_app.phonetic="/ˈsɜːveɪ/";frame.word_app.part_of_speech="n";frame.word_app.meaning="n. 调查；测验";frame.word_app.example="They conducted a survey.";frame.word_app.example_translation="他们进行了一项调查。";
    Render(frame,output,"pos-prefixed");CHECK(Seen("n. 调查；测验")&&!Seen("n n."));
    struct MeaningCase {const char* pos;const char* meaning;const char* expected;const char* duplicate;};
    for(const auto& item:std::vector<MeaningCase>{
        {"v.","v. 放弃；抛弃","v. 放弃；抛弃","v. v."},
        {"adj","adj. 不安的\nv. 使不安","adj. 不安的v. 使不安","adj adj."},
        {"n/v","n. 调查\nv. 调查","n. 调查v. 调查","n/v n."},
        {" v. "," \tV. 放弃\n","V. 放弃","v. V."},
        {"v","vt. 放弃","vt. 放弃","v vt."},
        {"n.","n.调查","n.调查","n. n."},
        {"a","a. 平静的","a. 平静的","a a."},
        {"n","nothing missing","n nothing missing","n n."},
        {"v.","放弃；抛弃","v. 放弃；抛弃","v. v."},
        {"","v. 放弃；抛弃","v. 放弃；抛弃","v. v."},
        {"v.","","暂无释义","v. 暂无释义"},
        {"v."," \t\n","暂无释义","v. 暂无释义"},
    }) {
        frame.word_app.part_of_speech=item.pos;frame.word_app.meaning=item.meaning;
        Render(frame,"","pos-regression");CHECK(Seen(item.expected)&&!Seen(item.duplicate));
    }
    frame.word_app=original_card;
    for(const std::string ipa:{"/ˈæpəl/","/θɪŋk/","/əˈbaʊt/","/ɜː/","/ɚɝʃʒɡɹɾʔ/"}){
        frame.word_app.phonetic=ipa;Render(frame,output,"ipa");CHECK(Seen(ipa));
        for(size_t i=0;i<ipa.size();){const auto c=WordNextCharacter(ipa,&i);CHECK(WordGlyph(16,c.text).second==c.text);}
    }
    frame.word_app.phonetic="/əˈbændən/";frame.word_app.save_failed=true;frame.word_app.feedback="暂存未保存，确认重试";Render(frame,output,"save-failed");CHECK(Seen("保存失败")&&Seen(frame.word_app.feedback));
    frame.word_app.save_failed=false;frame.word_app.saving=true;frame.word_app.feedback="正在重试保存";Render(frame,output,"saving");CHECK(Seen("保存中"));
    frame.word_app.saving=false;frame.word_app.feedback.clear();
    frame.word_app.word="pneumonoultramicroscopicsilicovolcanoconiosis";frame.word_app.phonetic="/ˌnjuːmənoʊˌʌltrəmaɪkrəˌskɒpɪkˌsɪlɪkoʊvɒlˌkeɪnoʊˈkoʊniːoʊsɪs/";
    frame.word_app.card_phase=WordCardPhase::kFront;Render(frame,output,"long-front");CHECK(Seen(frame.word_app.word));
    frame.word_app.card_phase=WordCardPhase::kRevealed;Render(frame,output,"long-back");CHECK(Seen("放弃；抛弃")&&Seen(frame.word_app.example));
    frame.word_app.word="résumé";frame.word_app.phonetic="/ˈrezjʊmeɪ/";Render(frame,output,"accented");CHECK(Seen("résumé"));
    frame.word_app.word="熟悉的单词";Render(frame,output,"cjk-headword");CHECK(Seen("熟悉的单词"));
    frame.word_app.word="abandon";frame.word_app.phonetic="/əˈbændən/";frame.word_app.meaning="放弃；抛弃；中止。指停止原有的行动或计划，也可表示离开本应照顾的人或事物。这个词常带有彻底停止、不再继续的语气，需要结合具体语境判断。";frame.word_app.example="They had to abandon their original plan because the weather was getting worse and the mountain road was no longer safe.";frame.word_app.example_translation="由于天气越来越糟、山路也不再安全，他们不得不放弃原来的计划，选择在附近的村庄等待天气好转。";Render(frame,output,"long-content");CHECK(Seen("..."));
    frame.word_app.mode=WordAppMode::kSessionStarting;frame.word_app.hint="正在加载后续单词";Render(frame,output,"loading");CHECK(Seen("正在准备本轮单词")&&Seen(frame.word_app.hint)&&!Seen("abandon"));
    frame.word_app.mode=WordAppMode::kReviewComplete;frame.word_app.review_complete_empty=false;frame.word_app.review_complete_reviewed=30;frame.word_app.review_complete_unknown=6;frame.word_app.review_complete_replayed=4;frame.word_app.sequential_total=3200;frame.word_app.sequential_cursor=128;Render(frame,output,"complete");CHECK(Seen("本轮复习完成")&&Seen("没答对 6 张")&&Seen("重学 4 张"));
    frame.word_app.review_complete_empty=true;Render(frame,output,"no-due");CHECK(Seen("今天没有到期单词")&&Seen("到期 0 词"));
    auto wrap=WordWrap("one internationalization two three",16,WordMeasure(16,"one internationalization"),3);CHECK(wrap.lines.size()>=2&&wrap.lines[0]=="one internationalization");
    wrap=WordWrap("first\nsecond\nthird",16,kWordWidth,2);CHECK(wrap.truncated&&wrap.lines.size()==2);
    CHECK(WordClip("x\ny\rz",12,368)=="x y z");CHECK(WordClip("abc",16,0).empty());
    CHECK(WordGlyph(16,"🧪").second=="□"||WordGlyph(16,"🧪").second=="?");
    CHECK(WordMeasure(16,WordGlyph(16,"🧪").second)>0);
    UiFrame changed=frame;const auto original=Signature(frame);
    ++changed.word_app.review_due_count;CHECK(Signature(changed)!=original);changed=frame;
    ++changed.word_app.mistake_count;CHECK(Signature(changed)!=original);changed=frame;
    changed.word_app.save_failed=!frame.word_app.save_failed;CHECK(Signature(changed)!=original);changed=frame;
    changed.word_app.scope_title="other";CHECK(Signature(changed)!=original);changed=frame;
    changed.home.battery_percent=42;CHECK(Signature(changed)!=original);
    frame.word_app.word="a/b";frame.word_app.phonetic="c";changed=frame;changed.word_app.word="a";changed.word_app.phonetic="b/c";CHECK(Signature(frame)!=Signature(changed));
    std::printf("PASS: %d word UI assertions, native glyphs/projection/signature; NOT HIL\n",checks);
}
