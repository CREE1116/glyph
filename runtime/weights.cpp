#include "weights.hpp"
#include "glyph/compiler.hpp"
#include "nlohmann/json.hpp"
#include <fstream>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <limits>
#ifdef __APPLE__
#include <Accelerate/Accelerate.h>
#else
#include <cblas.h>
#endif
namespace glyph::runtime {
using json=nlohmann::json;
static std::shared_ptr<Tensor> alloc(size_t r,size_t c){
 if(!r||!c||r>16000000/c)throw Error("model activation shape exceeds limit");
 auto t=std::make_shared<Tensor>();t->rows=r;t->cols=c;t->data.resize(r*c);return t;
}
std::shared_ptr<Tensor> load_weight(const std::string& file,const std::string& name,TensorContext& context){
 const auto key=file+"\n"+name;auto found=context.weights.find(key);if(found!=context.weights.end())return found->second;
 std::ifstream in(file,std::ios::binary);if(!in)throw Error("cannot read safetensors "+file);
 uint64_t length=0;in.read(reinterpret_cast<char*>(&length),8);
 if(!in||length<2||length>16*1024*1024)throw Error("invalid safetensors header length");
 std::string header(length,'\0');in.read(header.data(),length);if(!in)throw Error("truncated safetensors header");
 auto metadata=json::parse(header);if(!metadata.contains(name))throw Error("missing weight "+name);
 auto w=metadata.at(name);auto shape=w.at("shape").get<std::vector<size_t>>();
 if(shape.empty()||shape.size()>2)throw Error("weight requires rank 1 or 2");
 auto t=std::make_shared<Tensor>();t->rows=shape.size()==1?1:shape[0];t->cols=shape.back();
 if(!t->rows||!t->cols||t->rows>300000000/t->cols)throw Error("weight shape exceeds limit");
 const size_t count=t->rows*t->cols;std::string dtype=w.at("dtype");size_t size=dtype=="F32"?4:dtype=="BF16"||dtype=="F16"?2:0;
 if(!size)throw Error("unsupported safetensors dtype "+dtype);
 auto offsets=w.at("data_offsets").get<std::vector<uint64_t>>();
 if(offsets.size()!=2||offsets[1]<offsets[0]||offsets[1]-offsets[0]!=count*size)throw Error("invalid weight byte range");
 in.seekg(0,std::ios::end);auto end=in.tellg();if(end<0||uint64_t(end)<8+length||offsets[1]>uint64_t(end)-8-length)throw Error("truncated weight payload");
 in.seekg(8+length+offsets[0]);t->weights.resize(count);
 if(dtype=="F32")in.read(reinterpret_cast<char*>(t->weights.data()),count*4);
 else{std::vector<uint16_t> raw(count);in.read(reinterpret_cast<char*>(raw.data()),count*2);
 for(size_t i=0;i<count;++i){uint32_t bits;
 if(dtype=="BF16")bits=uint32_t(raw[i])<<16;
 else{uint32_t sign=(raw[i]&0x8000u)<<16,exp=(raw[i]>>10)&31,mant=raw[i]&1023;
 if(exp==0){if(!mant)bits=sign;else{int e=-14;while(!(mant&1024)){mant<<=1;--e;}bits=sign|uint32_t(e+127)<<23|(mant&1023)<<13;}}
 else if(exp==31)bits=sign|0x7f800000|mant<<13;else bits=sign|(exp+112)<<23|mant<<13;}
 std::memcpy(&t->weights[i],&bits,4);}}
 if(!in)throw Error("failed reading weight "+name);for(float f:t->weights)if(!std::isfinite(f))throw Error("nonfinite weight "+name);
 context.weights[key]=t;return t;
}
Value model_tensor_op(const std::string& op,std::vector<Value>& s,TensorContext& context){
 auto pop=[&](){if(s.empty())throw Error("model tensor stack underflow");auto v=s.back();s.pop_back();return v;};
 if(op=="TWEIGHT"){auto name=text_value(pop()),file=text_value(pop());return load_weight(file,name,context);}
 if(op=="TLINEAR"){auto w=tensor(pop()),x=tensor(pop());if(x->cols!=w->cols||w->weights.empty())throw Error("Linear weight shape mismatch");auto y=alloc(x->rows,w->rows);std::vector<float> a(x->data.begin(),x->data.end()),out(y->data.size());
 cblas_sgemm(CblasRowMajor,CblasNoTrans,CblasTrans,int(x->rows),int(w->rows),int(x->cols),1,a.data(),int(x->cols),w->weights.data(),int(w->cols),0,out.data(),int(w->rows));
 std::copy(out.begin(),out.end(),y->data.begin());return y;}
 if(op=="TWEMBED"){auto ids=tensor(pop()),w=tensor(pop());if(ids->cols!=1||w->weights.empty())throw Error("invalid weight embedding inputs");auto y=alloc(ids->rows,w->cols);for(size_t i=0;i<ids->rows;++i){double id=ids->data[i];if(id<0||id>=w->rows||id!=std::floor(id))throw Error("embedding token ID out of range");std::copy_n(w->weights.begin()+size_t(id)*w->cols,w->cols,y->data.begin()+i*w->cols);}return y;}
 if(op=="TBIAS"){auto w=tensor(pop()),x=tensor(pop());if(w->rows!=1||w->cols!=x->cols||w->weights.empty())throw Error("bias shape mismatch");auto y=alloc(x->rows,x->cols);for(size_t i=0;i<y->data.size();++i)y->data[i]=float(x->data[i])+w->weights[i%x->cols];return y;}
 if(op=="TWNORM"){double epsilon=std::get<double>(pop().data);auto w=tensor(pop()),x=tensor(pop());if(epsilon<=0||w->rows!=1||w->cols!=x->cols||w->weights.empty())throw Error("norm shape or epsilon invalid");auto y=alloc(x->rows,x->cols);for(size_t r=0;r<x->rows;++r){float sum=0;for(size_t j=0;j<x->cols;++j){float v=x->data[r*x->cols+j];sum+=v*v;}float scale=1/std::sqrt(sum/float(x->cols)+float(epsilon));for(size_t j=0;j<x->cols;++j)y->data[r*x->cols+j]=float(x->data[r*x->cols+j])*scale*w->weights[j];}return y;}
 if(op=="TMROPE"||op=="TMROPEAT"){int64_t offset=op=="TMROPEAT"?integer(pop()):0;double theta=std::get<double>(pop().data);auto heads=integer(pop());auto x=tensor(pop());
 if(heads<=0||x->cols%heads||((x->cols/heads)%2)||theta<=0)throw Error("invalid multi-head RoPE shape");
 if(offset<0||size_t(offset)+x->rows>context.cache_limit)throw Error("RoPE position offset out of range");
 size_t d=x->cols/heads;auto y=alloc(x->rows,x->cols);
 for(size_t r=0;r<x->rows;++r)for(int64_t h=0;h<heads;++h)for(size_t j=0;j<d/2;++j){float angle=float(double(offset)+double(r))*float(1/std::pow(theta,2.0*j/d));float co=std::cos(angle),si=std::sin(angle);size_t a=r*x->cols+h*d+j,b=a+d/2;float av=x->data[a],bv=x->data[b];y->data[a]=av*co-bv*si;y->data[b]=bv*co+av*si;}
 return y;}
 if(op=="TGQA"||op=="TGQACACHE"){
 std::string slot;if(op=="TGQACACHE")slot=text_value(pop());
 auto kv=integer(pop()),heads=integer(pop());auto v=tensor(pop()),k=tensor(pop()),q=tensor(pop());
 if(heads<=0||kv<=0||heads%kv||q->cols%heads||q->rows!=k->rows||k->rows!=v->rows||k->cols!=v->cols||k->cols!=size_t(kv)*(q->cols/heads))throw Error("GQA shape mismatch");
 if(op=="TGQACACHE"){if(slot.empty())throw Error("attention cache slot must be named");
  auto& entry=context.cache[slot];
  if(!entry.keys){entry.keys=k;entry.values=v;}
  else{if(entry.keys->cols!=k->cols||entry.values->cols!=v->cols)throw Error("attention cache slot shape changed");
   if(entry.keys->rows+k->rows>context.cache_limit)throw Error("attention cache exceeds context limit");
   auto keys=alloc(entry.keys->rows+k->rows,k->cols),values=alloc(entry.values->rows+v->rows,v->cols);
   std::copy(entry.keys->data.begin(),entry.keys->data.end(),keys->data.begin());
   std::copy(k->data.begin(),k->data.end(),keys->data.begin()+entry.keys->data.size());
   std::copy(entry.values->data.begin(),entry.values->data.end(),values->data.begin());
   std::copy(v->data.begin(),v->data.end(),values->data.begin()+entry.values->data.size());
   entry.keys=keys;entry.values=values;}
  k=entry.keys;v=entry.values;}
 // Row i of q sits at absolute position past+i and may read keys 0..past+i.
 size_t d=q->cols/heads,n=q->rows,m=k->rows,past=m-n;
 if(m<n)throw Error("attention cache shorter than the query block");
 auto out=alloc(n,q->cols);std::vector<float> scores(m);
 for(size_t i=0;i<n;++i)for(int64_t h=0;h<heads;++h){size_t kh=h/(heads/kv),last=past+i;float max=-std::numeric_limits<float>::infinity();
  for(size_t j=0;j<=last;++j){float sum=0;for(size_t t=0;t<d;++t)sum+=float(q->data[i*q->cols+h*d+t])*float(k->data[j*k->cols+kh*d+t]);scores[j]=sum/std::sqrt(float(d));max=std::max(max,scores[j]);}
  float denom=0;for(size_t j=0;j<=last;++j)denom+=(scores[j]=std::exp(scores[j]-max));
  for(size_t t=0;t<d;++t){float sum=0;for(size_t j=0;j<=last;++j)sum+=scores[j]/denom*float(v->data[j*v->cols+kh*d+t]);out->data[i*out->cols+h*d+t]=sum;}}
 return out;}
 throw Error("unknown model tensor opcode "+op);
}
}
