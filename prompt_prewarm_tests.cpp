// prompt_prewarm_tests.cpp -- standalone checks for prompt_prewarm.h.
// Build: cl /std:c++17 /EHsc prompt_prewarm_tests.cpp   (or g++ -std=c++17)

#include "prompt_prewarm.h"
#include <cstdio>
using namespace prompt_prewarm;
int fails=0;
#define EQ(a,b) do{auto _a=(a);auto _b=std::string(b); if(_a!=_b){printf("FAIL %s: got [%s] want [%s]\n",#a,_a.c_str(),_b.c_str());fails++;}}while(0)
#define T(x) do{if(!(x)){printf("FAIL %s\n",#x);fails++;}}while(0)
int main(){
 const std::string H="WORKING CONTEXT (this conversation)";
 const std::string big(3000,'x');
 // Qwen-style render: system block, then user, then generation prompt.
 std::string r="<|im_start|>system\n"+big+" see WORKING CONTEXT at the end.\n\n"+H+"\nCurrent working directory: C:\\x<|im_end|>\n<|im_start|>user\n.<|im_end|>\n<|im_start|>assistant\n";
 std::string p;
 T(CutStablePrefix(r,H,p)==Cut::Ok);
 T(p.size()>2 && p.substr(p.size()-2)=="\n\n");
 T(r.compare(0,p.size(),p)==0);
 T(p.find(H)==std::string::npos);
 // mention without the leading blank line is not the marker
 T(p.find("see WORKING CONTEXT")!=std::string::npos);
 // missing / repeated / too short
 T(CutStablePrefix("abc",H,p)==Cut::MarkerMissing && p.empty());
 T(CutStablePrefix(big+"\n\n"+H+"a\n\n"+H,H,p)==Cut::MarkerRepeated);
 T(CutStablePrefix("short\n\n"+H,H,p)==Cut::TooShort);
 T(CutStablePrefix(r,"",p)==Cut::MarkerMissing);
 // StablePart
 EQ(StablePart("A\n\n"+H+"\ncwd","" + H),"A");
 EQ(StablePart("no marker",H),"no marker");
 // keys: stable across per-chat changes, sensitive to real changes
 const std::string s1="SYS\n\n"+H+"\ncwd: C:\\a", s2="SYS\n\n"+H+"\ncwd: C:\\b";
 EQ(MakeKey("m","native","auto",StablePart(s1,H),"[]"), MakeKey("m","native","auto",StablePart(s2,H),"[]"));
 T(MakeKey("m","native","auto","SYS","[]")!=MakeKey("m","xml","auto","SYS","[]"));
 T(MakeKey("m","native","auto","SYS","[]")!=MakeKey("m2","native","auto","SYS","[]"));
 T(MakeKey("ab","c","","","")!=MakeKey("a","bc","","",""));   // separator matters
 T(MakeKey("m","n","a","s","t").size()==16);
 // JSON body
 EQ(BuildCompletionBody("a\"b\\c\nd\te\x01 \xC3\xB1"),
    "{\"prompt\":\"a\\\"b\\\\c\\nd\\te\\u0001 \xC3\xB1\",\"n_predict\":1,\"cache_prompt\":true,\"stream\":false,\"temperature\":0}");
 // Describe
 Outcome o; o.ok=true; o.promptTokens=13123; o.processed=13123; o.cachedTokens=0; o.promptMs=7900; o.wallMs=8050;
 EQ(Describe(o),"primed 13,123 tokens (processed 13,123 in 7.9 s), 8.1 s total");
 o.cachedTokens=12000; o.processed=1123; o.promptMs=700; o.wallMs=-1;
 EQ(Describe(o),"primed 13,123 tokens (processed 1,123, reused 12,000 in 0.7 s)");
 Outcome f; f.error="HTTP 404 from /apply-template";
 EQ(Describe(f),"failed: HTTP 404 from /apply-template");
 Outcome sk; sk.skipped=true; sk.error="superseded (a chat request took the slot first)";
 EQ(Describe(sk),"skipped: superseded (a chat request took the slot first)");
 EQ(WithCommas(999),"999"); EQ(WithCommas(1000),"1,000"); EQ(WithCommas(1234567),"1,234,567");
 printf(fails?"%d failures\n":"all passed\n",fails); return fails;
}
