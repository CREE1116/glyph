#include "qwen.hpp"
#include "glyph/compiler.hpp"
#include "nlohmann/json.hpp"
#include <filesystem>
#include <algorithm>
#include <iostream>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>
namespace glyph::runtime {
namespace fs=std::filesystem;using json=nlohmann::json;
static json config(const std::string& dir){auto j=json::parse(read_file((fs::path(dir)/"config.json").string()));if(j.value("model_type","")!="qwen2"||j.value("hidden_act","")!="silu"||j.value("use_sliding_window",false)||(j.contains("rope_scaling")&&!j["rope_scaling"].is_null()))throw Error("supported model: dense Qwen2/Qwen2.5 with SiLU, default RoPE, full attention");return j;}
std::string qwen_graph(const std::string& directory){
 auto dir=fs::absolute(directory);auto cfg=config(dir.string());int layers=cfg.at("num_hidden_layers"),heads=cfg.at("num_attention_heads"),kv=cfg.at("num_key_value_heads");int hidden=cfg.at("hidden_size");
 if(layers<1||layers>64||heads<1||kv<1||heads%kv||hidden%heads||(hidden/heads)%2)throw Error("invalid Qwen dimensions");
 auto weight_file=[&](const std::string& name){auto single=dir/"model.safetensors";if(fs::exists(single))return single.string();auto index=json::parse(read_file((dir/"model.safetensors.index.json").string()));std::string shard=index.at("weight_map").at(name);if(fs::path(shard).filename()!=shard)throw Error("invalid weight shard name");return (dir/shard).string();};
 auto weight=[&](const std::string& name){return "Weight.load("+quote(weight_file(name))+", "+quote(name)+")";};
 std::string declarations="# Imported Qwen transformer. Forward is executed by the Glyph VM.\n\n",steps;int counter=0;
 auto node=[&](std::string inputs,std::string expression,std::string args,std::string effect=""){std::string name="Op"+std::to_string(counter++),binding="v"+std::to_string(counter);declarations+="node "+name+"\n"+(inputs.empty()?"":"in:\n"+inputs)+"out:\n    Tensor\n"+(effect.empty()?"":"effect:\n    "+effect+"\n")+"impl:\n    "+expression+"\n\n";steps+=name+"("+args+") -> "+binding+"\n";return binding;};
 auto unary=[&](std::string expression,std::string arg,bool read=false){return node("    x: Tensor\n",expression,arg,read?"File.Read":"");};
 auto linear=[&](std::string input,std::string name,bool bias){auto result=unary("Tensor.Linear(x, "+weight(name+".weight")+")",input,true);if(bias)result=unary("Tensor.Bias(x, "+weight(name+".bias")+")",result,true);return result;};
 auto add=[&](std::string a,std::string b){return node("    a: Tensor\n    b: Tensor\n","Tensor.Add(a, b)",a+", "+b);};
 auto norm=[&](std::string input,std::string name){std::ostringstream eps;eps<<std::fixed<<std::setprecision(10)<<cfg.at("rms_norm_eps").get<double>();return unary("Tensor.Norm(x, "+weight(name)+", "+eps.str()+")",input,true);};
 auto x=unary("Tensor.Embed("+weight("model.embed_tokens.weight")+", x)","tokens",true);
 for(int l=0;l<layers;++l){auto prefix="model.layers."+std::to_string(l);auto n=norm(x,prefix+".input_layernorm.weight");auto q=linear(n,prefix+".self_attn.q_proj",true),k=linear(n,prefix+".self_attn.k_proj",true),v=linear(n,prefix+".self_attn.v_proj",true);auto theta=std::to_string(cfg.at("rope_theta").get<double>());
 // Positions come from the flow input, so a cached step can start past zero.
 auto rope=[&](std::string input,int groups){return node("    x: Tensor\n    offset: Int\n","Tensor.MultiRoPEAt(x, "+std::to_string(groups)+", "+theta+", offset)",input+", offset");};
 q=rope(q,heads);k=rope(k,kv);
 auto a=node("    q: Tensor\n    k: Tensor\n    v: Tensor\n","Tensor.AttendCached(q, k, v, "+std::to_string(heads)+", "+std::to_string(kv)+", "+quote("layer."+std::to_string(l))+")",q+", "+k+", "+v,"Model.Cache");
 x=add(x,linear(a,prefix+".self_attn.o_proj",false));n=norm(x,prefix+".post_attention_layernorm.weight");auto gate=unary("Tensor.SiLU(x)",linear(n,prefix+".mlp.gate_proj",false));auto up=linear(n,prefix+".mlp.up_proj",false);auto product=node("    a: Tensor\n    b: Tensor\n","Tensor.Mul(a, b)",gate+", "+up);x=add(x,linear(product,prefix+".mlp.down_proj",false));}
 x=norm(x,"model.norm.weight");x=unary("Tensor.Last(x)",x);linear(x,cfg.value("tie_word_embeddings",false)?"model.embed_tokens":"lm_head",false);
 return declarations+"flow Forward\nin:\n    tokens: Tensor\n    offset: Int\nout:\n    Tensor\n"+steps;
}
QwenModel::QwenModel(const std::string& dir):tokenizer_((fs::path(dir)/"tokenizer.json").string()){
 auto cfg=config(dir);vocab_size_=cfg.at("vocab_size");if(cfg.at("eos_token_id").is_array())eos_=cfg.at("eos_token_id").get<std::vector<int>>();else eos_={cfg.at("eos_token_id").get<int>()};
 auto source=qwen_graph(dir);auto c=compile(parse(source,"<qwen-model-graph>"));module_=decode(emit(c,"Forward"));
}
std::vector<int> QwenModel::tokenize(const std::string& s)const{return tokenizer_.encode(s);}
// One forward step over tokens[offset..]. The attention cache holds every earlier
// position, so a whole prompt (offset 0) and a single new token share one graph.
std::shared_ptr<Tensor> QwenModel::step(const std::vector<int>& ids,size_t offset){
 if(offset>=ids.size())throw Error("Qwen forward step needs at least one new token");
 auto t=std::make_shared<Tensor>();t->rows=ids.size()-offset;t->cols=1;t->data.assign(ids.begin()+offset,ids.end());
 auto out=tensor(run(module_,{Value(t),Value(std::int64_t(offset))},":memory:",&context_));
 if(out->rows!=1||out->cols!=vocab_size_)throw Error("model logits shape mismatch");
 for(auto value:out->data)if(!std::isfinite(value))throw Error("nonfinite Qwen logits");
 return out;
}
std::vector<double> QwenModel::logits(const std::string& text){auto ids=tokenize(text);if(ids.empty()||ids.size()>context_.cache_limit)throw Error("Qwen reference context supports 1..2048 tokens");context_.clear_cache();return step(ids,0)->data;}
std::vector<int> QwenModel::continue_greedy(const std::vector<int>& prompt,size_t count){
 if(prompt.empty()||prompt.size()+count>context_.cache_limit)throw Error("Qwen continuation exceeds 2048 tokens");
 auto ids=prompt;std::vector<int> generated;context_.clear_cache();size_t cached=0;
 for(size_t i=0;i<count;++i){auto values=step(ids,cached);cached=ids.size();
  int token=std::max_element(values->data.begin(),values->data.end())-values->data.begin();
  generated.push_back(token);ids.push_back(token);
  if(std::find(eos_.begin(),eos_.end(),token)!=eos_.end())break;}
 context_.clear_cache();return generated;
}
std::vector<double> QwenModel::cache_drift(const std::vector<int>& prompt,size_t count){
 auto ids=prompt;std::vector<std::vector<double>> cached;context_.clear_cache();size_t done=0;
 for(size_t i=0;i<count;++i){auto values=step(ids,done);done=ids.size();cached.push_back(values->data);
  ids.push_back(int(std::max_element(values->data.begin(),values->data.end())-values->data.begin()));}
 std::vector<double> drift;
 for(size_t i=0;i<cached.size();++i){std::vector<int> prefix(ids.begin(),ids.begin()+prompt.size()+i);
  context_.clear_cache();auto full=step(prefix,0);double worst=0;
  for(size_t j=0;j<full->data.size();++j)worst=std::max(worst,std::abs(full->data[j]-cached[i][j]));
  drift.push_back(worst);}
 context_.clear_cache();return drift;
}
std::string QwenModel::generate(const std::string& prompt){
 std::string chat="<|im_start|>system\nYou implement Glyph expressions. Output only the expression, no explanations or Markdown.<|im_end|>\n<|im_start|>user\n"+prompt+"<|im_end|>\n<|im_start|>assistant\n";
 auto ids=tokenizer_.encode(chat);if(ids.empty()||ids.size()+128>context_.cache_limit)throw Error("Qwen synthesis context exceeds 2048 tokens");std::string output;auto start=std::chrono::steady_clock::now();
 context_.clear_cache();size_t cached=0;
 for(int step=0;step<128;++step){auto values=this->step(ids,cached);cached=ids.size();
  int token=std::max_element(values->data.begin(),values->data.end())-values->data.begin();
  if(std::find(eos_.begin(),eos_.end(),token)!=eos_.end()){std::cerr<<"Qwen generated "<<step<<" tokens in "<<std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()<<"s\n";context_.clear_cache();return trim(output);}
  ids.push_back(token);output+=tokenizer_.decode(token);if(step%16==0)std::cerr<<"Qwen token "<<step+1<<"\n";}
 context_.clear_cache();throw Error("Qwen did not finish within 128 generated tokens");
}
}
