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
 printf(fails?"%d failures\n":"all passed\n",fails); return fails;
}
