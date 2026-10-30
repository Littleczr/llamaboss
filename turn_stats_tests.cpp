// turn_stats_tests.cpp -- standalone checks for turn_stats.h TSV read-back.
// Build: cl /std:c++17 /EHsc turn_stats_tests.cpp   (or g++ -std=c++17)

#include "turn_stats.h"
#include <cmath>
#include <cstdio>
int fails=0;
#define EQ(a,b) do{auto _a=(a);auto _b=std::string(b); if(_a!=_b){printf("FAIL %s: got [%s] want [%s]\n",#a,_a.c_str(),_b.c_str());fails++;}}while(0)
#define T(x) do{if(!(x)){printf("FAIL line %d: %s\n",__LINE__,#x);fails++;}}while(0)
static bool Near(double a,double b){return std::fabs(a-b)<0.01;}
int main(){
 // llama-server reply
 TurnStats a; a.firstByteMs=120; a.firstTokenMs=480; a.firstContentMs=900; a.lastTokenMs=8000; a.totalMs=8100;
 a.promptTokens=13200; a.completionTokens=512; a.hasServerTimings=true; a.serverPromptN=77; a.serverCacheN=13123;
 a.serverPredictedN=512; a.serverPromptMs=46.2; a.serverPredictedMs=7100; a.serverPredictedPerSec=72.11; a.serverPromptPerSec=1666.7;
 // remote reply with reasoning
 TurnStats b; b.firstTokenMs=2100; b.firstContentMs=5000; b.lastTokenMs=9000; b.totalMs=9100;
 b.promptTokens=14000; b.completionTokens=900; b.cachedPromptTokens=12800; b.reasoningTokens=400;
 std::string file=std::string(TurnStats::TsvHeader())+"\n"+a.TsvRow("2026-09-27 22:00:00","C:\\m\\qwen.gguf")+"\n"
   +b.TsvRow("2026-09-27 22:01:00","gpt-5.6-luna")+"\r\n\n";
 auto rows=TurnStats::ParseTsv(file,1000);
 T(rows.size()==2);
 const TurnStats& x=rows[0]; const TurnStats& y=rows[1];
 T(x.hasServerTimings && x.promptTokens==13200 && x.serverCacheN==13123 && x.serverPromptN==77);
 T(Near(x.GenerationTokensPerSec(),72.11) && Near(x.PromptTokensPerSec(),1666.70));
 T(Near(x.firstTokenMs,480) && Near(x.totalMs,8100));
 T(!y.hasServerTimings && y.cachedPromptTokens==12800 && y.reasoningTokens==400);
 T(Near(y.GenerationTokensPerSec(), b.GenerationTokensPerSec()));
 T(y.PromptTokensPerSec()<0);
 EQ(y.OneLineSummary(), b.OneLineSummary());
 EQ(x.OneLineSummary(), a.OneLineSummary());
 // cap keeps newest
 auto one=TurnStats::ParseTsv(file,1); T(one.size()==1 && one[0].promptTokens==14000);
 // wrong / empty files
 T(TurnStats::ParseTsv("garbage\n1\t2\t3\n",10).empty());
 T(TurnStats::ParseTsv("",10).empty());
 // older header with fewer columns + a repeated newer header
 std::string old="time\tmodel\tprompt_tokens\tcompletion_tokens\n2026\tm\t100\t20\n"+std::string(TurnStats::TsvHeader())+"\n"+a.TsvRow("t","m")+"\n";
 auto o=TurnStats::ParseTsv(old,10); T(o.size()==2 && o[0].promptTokens==100 && o[0].completionTokens==20 && !o[0].hasServerTimings && o[1].hasServerTimings);
 // malformed numbers -> -1, row with nothing usable dropped
 auto m=TurnStats::ParseTsv(std::string(TurnStats::TsvHeader())+"\nt\tm\tabc\n",10); T(m.empty());
 // live stats unaffected by the new field
 TurnStats c; c.completionTokens=101; c.firstTokenMs=0; c.lastTokenMs=1000; T(Near(c.GenerationTokensPerSec(),100));
 // 2026-10-11 columns: cache writes, cost, finish reason, reply shape round-trip
 TurnStats d; d.totalMs=3400; d.promptTokens=32204; d.completionTokens=568; d.cachedPromptTokens=31800;
 d.cacheWriteTokens=400; d.costUsd=0.0035; d.finishReason="stop"; d.thinkBytes=0; d.replyBytes=1890; d.toolCalls=0;
 auto dr=TurnStats::ParseTsv(std::string(TurnStats::TsvHeader())+"\n"+d.TsvRow("t","anthropic/claude-haiku-5.5")+"\n",10);
 T(dr.size()==1 && dr[0].cacheWriteTokens==400 && Near(dr[0].costUsd,0.0035) && dr[0].finishReason=="stop"
   && dr[0].thinkBytes==0 && dr[0].replyBytes==1890 && dr[0].toolCalls==0);
 T(d.OneLineSummary().find("400 cache write")!=std::string::npos);
 // unset new fields stay blank and read back as unset
 T(x.cacheWriteTokens==-1 && x.costUsd<0 && x.finishReason.empty() && x.toolCalls==-1);
 // a tab inside finish_reason cannot break the row
 TurnStats e=d; e.finishReason="a\tb"; auto er=TurnStats::ParseTsv(std::string(TurnStats::TsvHeader())+"\n"+e.TsvRow("t","m")+"\n",10);
 T(er.size()==1 && er[0].finishReason=="a b" && er[0].toolCalls==0);
 // repetition stop: no usage, no finish_reason, flag survives the round trip
 TurnStats s; s.firstTokenMs=1100; s.totalMs=42200; s.thinkBytes=17900; s.replyBytes=0; s.toolCalls=0;
 s.stoppedForRepetition=true;
 std::string srow=s.TsvRow("t","m");
 T(srow.size()>11 && srow.compare(srow.size()-11,11,"\trepetition")==0);
 auto sr=TurnStats::ParseTsv(std::string(TurnStats::TsvHeader())+"\n"+srow+"\n",10);
 T(sr.size()==1 && sr[0].stoppedForRepetition && sr[0].promptTokens==-1 && sr[0].finishReason.empty());
 // normal reply: stopped_by blank, reads back false; old headers without the column read false
 T(!dr[0].stoppedForRepetition && d.TsvRow("t","m").back()=='\t');
 T(!o[0].stoppedForRepetition && !o[1].stoppedForRepetition);
 // header and row stay the same width
 auto cols=[](const std::string& l){ size_t n=1; for(char ch:l) if(ch=='\t') ++n; return n; };
 T(cols(TurnStats::TsvHeader())==cols(srow) && cols(srow)==cols(d.TsvRow("t","m")));
 printf(fails?"%d failures\n":"all passed\n",fails); return fails;
}
