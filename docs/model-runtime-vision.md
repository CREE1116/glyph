그게 오히려 Glyph의 구조를 완성시켜. **LLM Compiler가 사용하는 기본 모델조차 Glyph Runtime 위에서 실행되는 것**으로 잡으면 돼.

즉 처음에는 C로 최소 커널을 부트스트랩하지만, 그 위의 Tensor/NN/Inference는 전부 Glyph의 계산 그래프로 표현하는 거야.

```text id="n8ufcm"
                 Glyph Compiler
                       │
                 unresolved Node
                       │
                       ▼
                GlyphCoder Tiny
                       │
            ┌──────────┴──────────┐
            │   Glyph Model Graph │
            │                     │
            │ Embedding           │
            │    ↓                │
            │ Attention           │
            │    ↓                │
            │ FFN                 │
            │    ↓                │
            │ LM Head             │
            └──────────┬──────────┘
                       │
                       ▼
                Glyph Runtime
                       │
          ┌────────────┼────────────┐
          ▼            ▼            ▼
        CPU          GPU         Accelerator
          │
          ▼
       C Kernel
```

그러면 **PyTorch나 llama.cpp 같은 별도의 모델 런타임이 필수 의존성이 아니게 되는 것**이지.

## Glyph에서 모델도 그냥 Flow

예를 들어 Transformer layer 자체를 개념적으로:

```text id="w73nm6"
flow TransformerBlock

in:
    Tensor<Input>
    KVCache

out:
    Tensor<Output>
    KVCache

LayerNorm
Attention
Residual
LayerNorm
MLP
Residual
```

라고 표현할 수 있고,

`Attention`도:

```text id="ur3dce"
flow Attention

LinearQ
LinearK
LinearV

RoPE

Q + K
    -> AttentionScore
    -> Softmax
    -> V

LinearOut
```

결국 전부 일반 Glyph Node야.

```text id="iz5kbk"
Tensor.MatMul
Tensor.Add
Tensor.Mul
Tensor.Reshape
Tensor.Softmax
Tensor.RMSNorm
Tensor.RoPE
Tensor.SiLU
```

이런 primitive만 runtime에서 충분히 빠르게 제공하면 모델 전체를 Glyph Graph로 올릴 수 있어.

---

## 그러면 PyTorch 기능을 지원한다는 의미도 달라짐

단순히:

> Glyph에서 PyTorch를 호출할 수 있다.

가 아니라 장기적으로는:

> **PyTorch가 표현하는 계산 그래프를 Glyph 자체가 표현하고 실행할 수 있다.**

가 되는 거야.

초기에는:

```text id="g9cjww"
PyTorch
   ↓ import
Glyph Model Graph
   ↓
Glyph Runtime
```

형태로 갈 수도 있고.

예를 들어 ONNX 같은 중간 표현을 import해서:

```text id="pye94z"
model.onnx
    ↓
Glyph importer
    ↓
Glyph Tensor Graph
```

로 만드는 것도 가능하고.

---

## 이러면 굉장히 재밌는 자기호스팅 구조가 생김

Glyph Compiler가 사용하는 LLM:

```text id="5wtbh2"
GlyphCoder
```

자체가 Glyph 프로그램인 거야.

```text id="9hd6we"
Glyph Source
     ↓
Graph Compiler
     ↓
LLM Compiler
     │
     └── GlyphCoder
           ↓
        Glyph Runtime
     ↓
Resolved Glyph
     ↓
Executable Compiler
```

즉 **Glyph가 Glyph를 만드는 모델을 실행한다.**

완전한 self-hosting은 아니지만 일종의 AI 부트스트래핑 구조지.

C가 담당하는 건 점점 아주 얇은 기반만 남는다.

```text id="xf32js"
C Kernel
────────────────
Memory
Threads
SIMD
GPU interface
File / mmap
OS interface
Primitive Tensor Kernels
────────────────

Glyph Runtime
────────────────
Tensor Graph
Model Runtime
Scheduler
KV Cache
Sampling
Tokenizer
Model Loading
────────────────

Glyph Programs
────────────────
GlyphCoder
Applications
AI Models
────────────────
```

이렇게.

---

## 특히 계산 그래프를 이미 갖고 있다는 게 엄청 유리함

일반적인 LLM runtime도 결국 하는 일은 대부분 그래프 실행이니까.

```text id="1lgwdb"
Weights
  ↓
MatMul
  ↓
Norm
  ↓
Attention
  ↓
MatMul
  ↓
Activation
  ↓
...
```

Glyph Compiler가 이미 Graph를 알고 있으면:

```text id="45bzvj"
kernel fusion
memory planning
buffer reuse
parallel execution
constant folding
device placement
quantization lowering
```

같은 최적화를 기존 프로그램과 **똑같은 compiler infrastructure**에서 할 수 있어.

특히 우리가 앞에서 메모리 관리를 자동화한다고 했잖아.

모델 실행에서는 오히려 일반 GC보다 더 강하게 할 수 있어.

```text id="xhd5yr"
Tensor A
   ↓
Node X
   ↓
Node Y
   ↓
last use
   ↓
buffer reuse
```

Graph Compiler가 Tensor lifetime을 정확히 알면 GPU/CPU 버퍼를 재사용할 수 있어.

그래서 모델 내부 Tensor까지 GC에 맡길 필요도 별로 없어.

```text id="x00ijh"
Tensor graph
→ static lifetime analysis

Application object
→ Arena / GC
```

로 나누면 된다.

---

## KV Cache도 Glyph의 Resource/State로 표현 가능

LLM inference에서 특수한 부분 중 하나인데 이것도 기존 개념에 들어가.

```text id="alrnc1"
state KVCache temporary {
    key: Tensor
    value: Tensor
}
```

또는 더 특수하게:

```text id="rxin1h"
resource KVCache
```

그리고:

```text id="72js8f"
Token
  ↓
Model.Forward
  ↕ KVCache
  ↓
Logits
  ↓
Sampler
  ↓
Token
```

이게 그냥 하나의 Flow가 돼.

샘플링도 Node.

```text id="56pct0"
Logits
 → Temperature
 → TopK
 → TopP
 → Sample
 → Token
```

그래서 inference runtime조차 Glyph 언어 철학 밖의 예외가 거의 없어져.

---

## 모델 파일도 Package로 취급 가능

예를 들면:

```text id="q6329x"
model GlyphCoderTiny

architecture:
    Transformer

weights:
    glyphcoder-1b.q4

dtype:
    Q4

context:
    8192

runtime:
    glyph
```

사용할 때는:

```text id="0q9jmp"
AI.Generate
    model GlyphCoderTiny
```

정도.

실제로는 compiler 내부에서도 같은 runtime API를 쓸 수 있고.

---

## 아주 중요한 장점: Python을 개발 경로에서 제거 가능

현재 AI 개발 스택은 흔히:

```text id="p31nj0"
Python
→ PyTorch
→ CUDA
→ C++
→ GPU
```

인데 Glyph가 목표로 하는 건:

```text id="wr566g"
Glyph
→ Glyph Graph
→ Glyph Runtime
→ Native Kernel
→ CPU/GPU
```

이 될 수 있어.

그러면 일반 앱 개발과 AI 개발이 **같은 언어, 같은 타입 시스템, 같은 그래프, 같은 빌드 시스템**을 쓴다.

예를 들어 웹 서비스에서:

```text id="vbkw1i"
HTTP.Request
    ↓
DB.User.Read
    ↓
Tokenizer
    ↓
Model
    ↓
Sampler
    ↓
DB.Log.Write
    ↓
HTTP.Response
```

전체가 한 Graph야.

이건 꽤 강력한 그림이지.

---

다만 여기서는 **C Kernel의 역할을 정확하게 제한하는 것**이 중요할 것 같아.

`MatMul` 같은 것까지 전부 순수 Glyph로 구현하려고 하면 성능을 잃어버려.

그래서:

```text id="d88qc0"
Glyph
    = graph / semantics / orchestration

Native Kernel
    = hot primitive
```

로 분리해야 해.

예를 들어:

```text id="n6d5x1"
Tensor.MatMul
```

은 Glyph Node지만 backend는:

```text id="rhxnrh"
CPU AVX/NEON kernel
CUDA kernel
Metal kernel
```

중 하나.

`TransformerBlock`은 Glyph Flow.

즉 **작은 연산은 Native, 큰 구조는 Glyph**.

이게 딱 맞아.

---

그러면 Glyph v0.1 이후의 런타임 계층을 이렇게 정의할 수 있을 것 같아.

```text id="diz9n5"
Layer 0
Native Kernel
C / SIMD / CUDA / Metal

Layer 1
Glyph Runtime
Memory / Scheduler / Tensor / State / Resource

Layer 2
Standard Node Graphs
NN / DB / HTTP / Image / Crypto

Layer 3
Glyph Model Runtime
Tokenizer / Transformer / KV / Sampler

Layer 4
GlyphCoder
Node synthesis model

Layer 5
User Glyph Programs
```

그리고 이 구조에서 아주 상징적인 목표 하나를 잡을 수 있어.

> **`glyph synth`를 실행하는 모델이 Glyph 자체로 실행된다.**

그 순간 Glyph는 단순히 “AI를 이용하는 프로그래밍 언어”가 아니라 **자기 자신의 AI 컴파일러까지 실행하는 AI-native 컴퓨팅 런타임**이 돼.
