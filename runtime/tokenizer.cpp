#include "tokenizer.hpp"
#include "glyph/compiler.hpp"
#include "nlohmann/json.hpp"
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <unordered_map>
#include <map>
#include <limits>
#include <algorithm>
namespace glyph::runtime {
namespace {
std::string utf8(int cp){std::string s;if(cp<128)s+=char(cp);else if(cp<2048){s+=char(0xc0|(cp>>6));s+=char(0x80|(cp&63));}else{s+=char(0xe0|(cp>>12));s+=char(0x80|((cp>>6)&63));s+=char(0x80|(cp&63));}return s;}
std::vector<std::string> characters(const std::string& s){std::vector<std::string> result;for(size_t i=0;i<s.size();){unsigned char c=s[i];size_t n=c<128?1:c<224?2:c<240?3:4;if(i+n>s.size())throw Error("invalid UTF-8 vocabulary");result.push_back(s.substr(i,n));i+=n;}return result;}
}
struct QwenTokenizer::Impl {
 std::unordered_map<std::string,int> vocab,ranks,special;
 std::map<int,std::string> inverse;
 std::string bytes[256];std::unordered_map<std::string,char> byte_inverse;
 pcre2_code* regex=nullptr;
 ~Impl(){if(regex)pcre2_code_free(regex);}
};
QwenTokenizer::QwenTokenizer(const std::string& file):impl_(std::make_unique<Impl>()){
 auto j=nlohmann::json::parse(read_file(file));auto& p=*impl_;
 if(j.at("model").at("type")!="BPE")throw Error("Qwen tokenizer requires BPE");
 for(auto it=j["model"]["vocab"].begin();it!=j["model"]["vocab"].end();++it){int id=it.value();p.vocab[it.key()]=id;p.inverse[id]=it.key();}
 int rank=0;for(const auto& merge:j["model"]["merges"]){std::string pair;if(merge.is_string())pair=merge.get<std::string>();else pair=merge[0].get<std::string>()+" "+merge[1].get<std::string>();p.ranks[pair]=rank++;}
 for(const auto& token:j["added_tokens"]){std::string text=token["content"];int id=token["id"];p.special[text]=id;p.inverse[id]=text;}
 int next=256;for(int b=0;b<256;++b){int cp=((b>=33&&b<=126)||(b>=161&&b<=172)||(b>=174))?b:next++;p.bytes[b]=utf8(cp);p.byte_inverse[p.bytes[b]]=char(b);}
 // Qwen2 byte-BPE pre-tokenizer. Unicode properties require PCRE2 UTF/UCP.
 const std::string pattern=R"((?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?\p{L}+|\p{N}| ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+)";
 int error;PCRE2_SIZE offset;p.regex=pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.c_str()),pattern.size(),PCRE2_UTF|PCRE2_UCP,&error,&offset,nullptr);
 if(!p.regex)throw Error("cannot compile Qwen tokenizer regex");
}
QwenTokenizer::~QwenTokenizer()=default;
std::vector<int> QwenTokenizer::encode(const std::string& text)const{
 auto& p=*impl_;std::vector<int> ids;
 auto ordinary=[&](const std::string& chunk){
 std::unique_ptr<pcre2_match_data,decltype(&pcre2_match_data_free)> match(pcre2_match_data_create_from_pattern(p.regex,nullptr),pcre2_match_data_free);
 size_t pos=0;while(pos<chunk.size()){
 int rc=pcre2_match(p.regex,reinterpret_cast<PCRE2_SPTR>(chunk.data()),chunk.size(),pos,PCRE2_ANCHORED,match.get(),nullptr);
 if(rc<0)throw Error("tokenizer cannot split input (invalid UTF-8?)");auto range=pcre2_get_ovector_pointer(match.get());if(range[1]<=pos)throw Error("empty tokenizer match");
 std::vector<std::string> pieces;for(size_t i=pos;i<range[1];++i)pieces.push_back(p.bytes[static_cast<unsigned char>(chunk[i])]);pos=range[1];
 while(pieces.size()>1){int best=std::numeric_limits<int>::max();size_t index=0;for(size_t i=0;i+1<pieces.size();++i){auto it=p.ranks.find(pieces[i]+" "+pieces[i+1]);if(it!=p.ranks.end()&&it->second<best){best=it->second;index=i;}}if(best==std::numeric_limits<int>::max())break;pieces[index]+=pieces[index+1];pieces.erase(pieces.begin()+index+1);}
 for(const auto& piece:pieces){auto it=p.vocab.find(piece);if(it==p.vocab.end())throw Error("BPE token missing from vocabulary");ids.push_back(it->second);}
 }};
 size_t at=0;while(at<text.size()){size_t nearest=text.size();std::string chosen;int id=0;for(const auto& [token,value]:p.special){auto where=text.find(token,at);if(where<nearest||(where==nearest&&token.size()>chosen.size())){nearest=where;chosen=token;id=value;}}ordinary(text.substr(at,nearest-at));if(chosen.empty())break;ids.push_back(id);at=nearest+chosen.size();}return ids;
}
std::string QwenTokenizer::decode(int token)const{
 auto& p=*impl_;auto it=p.inverse.find(token);if(it==p.inverse.end())throw Error("unknown token ID");if(p.special.count(it->second))return it->second;std::string result;for(const auto& cp:characters(it->second)){auto b=p.byte_inverse.find(cp);if(b==p.byte_inverse.end())throw Error("invalid byte-BPE vocabulary token");result+=b->second;}return result;
}
}
