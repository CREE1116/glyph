# Glyph Language v0.1

## 0. 개요

**Glyph**는 프로그램의 구조와 절차를 `Flow Graph`로 표현하고, 각 실행 단위를 `Node`로 구성하는 고수준 프로그래밍 언어다.

Glyph의 핵심 목표는 세 가지다.

1. 인간이 작성해야 하는 코드를 최소화한다.
2. 컴파일러가 처리할 수 있는 일은 LLM에게 맡기지 않는다.
3. LLM이 필요한 경우에도 전체 프로그램이 아니라 작은 Node 하나만 구현하게 한다.

Glyph는 LLM이 없어도 완전한 프로그래밍 언어로 동작한다.

LLM은 선택적인 **코드 합성 컴파일러**다.

핵심 원칙:

> **Compiler understands the program.
> LLM implements the node.**

또는:

> **Deterministic where possible, generative where necessary.**

---

# 1. 핵심 용어

Glyph의 기본 개념은 다음과 같다.

```text
Glyph
 ├─ Flow
 ├─ Node
 ├─ Type
 ├─ State
 ├─ Effect
 ├─ Resource
 ├─ Contract
 └─ Package
```

각 용어의 의미:

```text
Glyph
    프로그래밍 언어 전체

Flow
    프로그램의 절차적 흐름

Node
    최소 의미 실행 단위

Graph
    Flow와 Node의 전체 연결 구조

Contract
    Node의 입력·출력·효과·실패·불변식 규약

Intent
    정형화하기 어려운 의미를 표현하는 자연어 명세

State
    프로그램이 지속적으로 보존하는 상태

Package
    재사용 가능한 Node와 Type의 묶음
```

---

# 2. 프로그램 모델

Glyph 프로그램은 본질적으로 **Typed Flow Graph**다.

예:

```text
Request
   ↓
Parse
   ↓
Validate
   ↓
LoadUser
   ↓
Calculate
   ↓
Save
   ↓
Response
```

컴파일러는 프로그램을 다음 그래프로 동시에 해석할 수 있다.

```text
Control Flow Graph
Data Flow Graph
State Graph
Effect Graph
Error Graph
Resource Graph
Dependency Graph
Synthesis Dependency Graph
```

사용자는 이 전체 구조를 직접 관리하지 않는다.

Glyph Compiler가 생성한다.

---

# 3. Flow

`Flow`는 Node들이 어떤 절차로 연결되는지를 표현한다.

예:

```text
flow Register

in:
    RegisterRequest

out:
    User

ValidateRequest
CheckDuplicate
CreateUser
SaveUser
```

위 코드는 절차를 의미한다.

```text
RegisterRequest
      ↓
ValidateRequest
      ↓
CheckDuplicate
      ↓
CreateUser
      ↓
SaveUser
      ↓
User
```

Node 사이의 연결이 타입만으로 유일하게 결정된다면 명시적인 변수나 wiring은 필요하지 않다.

필요한 경우에만 연결을 직접 지정한다.

```text
ValidateRequest.output -> CheckDuplicate.input
```

---

# 4. Node

Node는 Glyph의 최소 실행 단위다.

Node는 함수와 비슷할 수 있지만 함수보다 넓은 개념이다.

Node는 다음 중 무엇이든 될 수 있다.

```text
계산
데이터 변환
분기
검증
DB 조회
DB 갱신
파일 접근
HTTP 요청
GPU 연산
LLM inference
외부 API
Native library 호출
```

Node의 기본 구조:

```text
node CreateUser

in:
    ValidRequest

out:
    User

ensure:
    output.id exists
    output.created_at exists

intent:
    검증된 요청을 기반으로 신규 사용자를 생성한다.
```

---

# 5. Node Contract

Node는 구현보다 계약이 우선이다.

Node Contract의 전체 구성은 다음과 같다.

```text
Input
Output
Precondition
Postcondition
Effect
State Read
State Write
Failure
Resource
Invariant
Allowed Dependency
Implementation
```

예:

```text
node ResizeImage

in:
    image: Image
    width: Int > 0
    height: Int > 0

out:
    Image

ensure:
    output.width == width
    output.height == height

effect:
    none

fail:
    InvalidImage
    InvalidSize
```

Node implementation은 opaque하다.

Compiler는 구현 방식보다 Contract를 신뢰한다.

---

# 6. 자연어 Intent

Glyph에서 자연어는 프로그램 자체가 아니다.

자연어는 **정형 계약으로 표현하기 어려운 의미적 요구사항**만 표현한다.

예:

```text
node Recommend

in:
    User
    Products

out:
    List<Product>

ensure:
    output.size <= 10
    output excludes purchased products

intent:
    최근 행동을 더 강하게 반영하되
    하나의 카테고리에 추천이 지나치게 몰리지 않도록 한다.
```

원칙:

> 구조화 가능한 것은 Glyph 문법으로 작성한다.
> 의미적인 빈칸만 자연어로 남긴다.

---

# 7. 변수 모델

Glyph에서는 기본적으로 mutable local variable을 사용하지 않는다.

값은 Flow Graph의 Edge다.

전통적 코드:

```text
user = load_user(id)
valid = verify(user)
profile = build_profile(user)
```

Glyph의 내부 모델:

```text
Id
 ↓
LoadUser
 ↓ User
Verify
 ↓ VerifiedUser
BuildProfile
 ↓ Profile
```

필요하면 값에 이름을 붙일 수 있다.

```text
LoadUser(UserId) -> user
LoadPolicy(user) -> policy
Authorize(user, policy)
```

하지만 이는 mutable variable이 아니라 **immutable binding**이다.

재할당은 기본적으로 존재하지 않는다.

---

# 8. State

변경 가능한 값은 local variable이 아니라 `State`로 표현한다.

예:

```text
state User persistent {
    id: Id
    email: Email unique
    name: Text
    created_at: Time
}
```

State는 단순 데이터 타입이 아니다.

Compiler는 State의:

```text
Schema
Read dependency
Write dependency
Transaction
Migration
Invariant
Persistence
```

를 추적한다.

---

# 9. DB 내장 지원

Glyph는 DB를 외부 ORM이 아니라 **언어 자체의 State backend**로 취급한다.

예:

```text
store main:
    postgres
```

그리고:

```text
state User @main {
    id: Id
    email: Email unique
}
```

조회 Node:

```text
node FindUser

in:
    Email

out:
    User?

read:
    User

where:
    User.email == input
```

쓰기 Node:

```text
node SaveUser

in:
    User

out:
    User

write:
    User
```

일반적인 Glyph 사용자는 다음을 직접 다루지 않는다.

```text
SQL
ORM
Connection Pool
Prepared Statement
Transaction boilerplate
```

---

# 10. State 종류

v0.1에서 다음 State 종류를 정의한다.

```text
persistent
ephemeral
temporary
append
```

의미:

```text
persistent
    영구 저장

ephemeral
    세션 또는 캐시

temporary
    Flow 실행 동안만 존재

append
    append-only event/log
```

---

# 11. Transaction

Flow 자체를 atomic하게 선언할 수 있다.

```text
flow Checkout atomic

ValidateOrder
ReserveInventory
CreatePayment
CreateOrder
```

DB State는 자동 transaction으로 묶인다.

외부 시스템처럼 rollback이 불가능한 Node는 compensation을 가질 수 있다.

```text
node ChargePayment

effect:
    ExternalState
    Network

compensate:
    RefundPayment
```

---

# 12. Effect System

Glyph는 Node의 부작용을 Compiler가 추적한다.

기본 Effect:

```text
IO
Network
DB.Read
DB.Write
File.Read
File.Write
Clock
Random
Process
GPU
ModelInference
ExternalState
```

Pure Node:

```text
node NormalizeText

in:
    Text

out:
    Text

effect:
    none
```

HTTP Node:

```text
node GetWeather

in:
    Location

out:
    Weather

effect:
    Network
```

LLM Compiler는 Node Contract에 허용되지 않은 Effect를 생성할 수 없다.

---

# 13. Error Flow

Glyph의 오류는 Exception보다 명시적인 Failure Edge를 기본으로 한다.

```text
node LoadUser

in:
    UserId

out:
    User

fail:
    NotFound
    DBTimeout
```

Graph:

```text
LoadUser
 ├─ success   → Authorize
 ├─ NotFound  → UserNotFound
 └─ DBTimeout → Retry
```

처리되지 않은 Failure는 Graph Compiler가 탐지한다.

---

# 14. 메모리 모델

Glyph 사용자는 일반적인 메모리 관리를 하지 않는다.

일반 코드에서 다음 개념은 노출하지 않는다.

```text
malloc
free
pointer
manual lifetime
manual ownership
```

기본 메모리 모델:

```text
Primitive
→ register / stack

Node-local temporary
→ Node Arena

Flow-local object
→ Flow Arena

Escaping object
→ GC Heap

Persistent object
→ State Backend

Native resource
→ Managed Handle
```

---

# 15. GC

Glyph Runtime은 기본적으로 GC를 지원한다.

하지만 모든 객체를 GC에 맡길 필요는 없다.

Compiler가 lifetime을 증명할 수 있다면:

```text
Node Arena
Flow Arena
Static Lifetime
```

으로 처리한다.

증명할 수 없는 escaping object만 GC Heap에 배치한다.

즉 사용자 관점에서는 GC 언어지만 내부 구현은 hybrid memory model이다.

---

# 16. Resource

파일, 네트워크 소켓, GPU 메모리, 모델 등의 자원은 Managed Handle로 표현한다.

```text
Handle<File>
Handle<Socket>
Handle<Model>
Handle<Tensor>
```

사용자는 일반적으로:

```text
open
close
free
```

를 작성하지 않는다.

Runtime이 deterministic resource lifetime을 관리한다.

---

# 17. Tensor / AI 기본 지원

Glyph는 계산 그래프를 Compiler가 이미 관리하기 때문에 수치계산 및 AI 개발을 주요 영역으로 지원한다.

기본 Type:

```text
Tensor
Shape
DType
Device
Dataset
Batch
Model
Optimizer
Loss
Gradient
```

기본 Tensor Node 예:

```text
Tensor.MatMul
Tensor.Add
Tensor.Mul
Tensor.Reshape
Tensor.Concat
Tensor.Slice
Tensor.Reduce
Tensor.Softmax
```

Neural Node:

```text
NN.Linear
NN.Conv1D
NN.Conv2D
NN.Embedding
NN.LayerNorm
NN.Dropout
NN.ReLU
NN.GELU
NN.Attention
```

Training Node:

```text
Train.Forward
Train.Loss
Train.Backward
Train.Step
Train.ZeroGrad
```

Optimizer:

```text
Optim.SGD
Optim.Adam
Optim.AdamW
```

Dataset:

```text
Data.Load
Data.Map
Data.Batch
Data.Shuffle
Data.Split
```

---

# 18. 계산 그래프

예:

```text
Tensor
   ↓
NN.Linear
   ↓
NN.GELU
   ↓
NN.Linear
   ↓
Loss
   ↓
Gradient
   ↓
Optim.Adam
```

Glyph Compiler는 이 그래프를 직접 알고 있으므로 추후 다음 최적화를 지원할 수 있다.

```text
operator fusion
constant folding
memory reuse
dead tensor elimination
automatic batching
device placement
parallel scheduling
kernel selection
```

v0.1에서는 기본 CPU backend부터 구현해도 된다.

GPU backend는 후속 단계다.

---

# 19. Node 구현 언어

Node가 어떤 언어로 작성됐는지는 Glyph 의미론과 무관하다.

허용 가능한 backend:

```text
Glyph
C
C++
Rust
Python
WASM
SQL
HTTP
GPU Kernel
Remote Service
```

모든 Node는 Manifest Contract를 통해 Glyph 세계에 들어온다.

---

# 20. Node Manifest

예:

```text
package image.core
version 1.0

node Image.Resize

in:
    Image
    Width
    Height

out:
    Image

effect:
    none

backend:
    c:
        library: libimage
        symbol: image_resize
```

Python backend:

```text
backend:
    python:
        module: image_adapter
        symbol: resize
```

사용자는 구현 언어를 알 필요가 없다.

---

# 21. Library Model

Glyph의 라이브러리 단위는 `Node Package`다.

Package 구성:

```text
Node
Type
Contract
Effect
Failure
State metadata
Backend binding
Compiler metadata
```

기존 라이브러리는 다시 작성하지 않는다.

```text
Existing Library
       ↓
Adapter
       ↓
Node Manifest
       ↓
Glyph Package
```

---

# 22. Foreign Interface

v0.1 외부 연결 목표:

```text
C ABI
Rust C ABI
Python Bridge
HTTP/OpenAPI
WASM
```

기본 공통 ABI는 C ABI를 사용한다.

---

# 23. Glyph Runtime Kernel

Runtime Kernel은 C 구현을 기본안으로 한다.

구성:

```text
FlowValue
FlowType
FlowHandle
FlowContext

Node ABI
Graph Executor
Scheduler

GC
Arena Allocator
Resource Manager

Error Runtime
State Runtime
FFI
```

---

# 24. Compiler 구현

Compiler frontend는 Rust 구현을 권장한다.

```text
glyphc
    Rust

libglyph
    C

Glyph ABI
    C

Standard Nodes
    C / Rust
```

Compiler와 Runtime 구현 언어는 Glyph 프로그램의 의미론과 무관하다.

---

# 25. 3단 컴파일 구조

Glyph의 핵심 빌드 모델은 다음 세 단계다.

```text
Human Glyph Code
      ↓
① Graph Compile
      ↓
Graph-Resolved Code
      ↓
② LLM Compile
      ↓
Fully-Resolved Glyph Code
      ↓
③ Executable Compile
      ↓
Executable
```

첫 두 단계 결과는 모두 **Code Artifact**다.

마지막 단계에서만 실행파일로 변환된다.

---

# 26. Phase 1 — Graph Compile

명령:

```text
glyph graph
```

입력:

```text
Human-authored Glyph Source
```

출력:

```text
Graph-Resolved Glyph Code
```

이 단계에는 LLM이 존재하지 않는다.

Compiler가 수행하는 작업:

```text
Parsing
Symbol Resolution
Type Resolution

Control Flow
Data Flow
State Flow
Effect Flow
Error Flow

Dependency Graph
Synthesis Dependency Graph
Contract Resolution
Invariant Analysis
```

---

# 27. Graph Compile 예

사용자 코드:

```text
flow Register

Validate
CheckDuplicate
CreateUser
Save
```

Graph Compile 이후:

```text
node Validate
in RegisterRequest
out ValidRequest
next CheckDuplicate

node CheckDuplicate
in ValidRequest
out UniqueRequest
read User
fail DuplicateUser
next CreateUser

node CreateUser
in UniqueRequest
out User
unresolved
next Save

node Save
in User
out User
write User
```

이 상태는 이미 구조적으로 완전한 코드다.

다만 일부 Node implementation이 비어 있을 수 있다.

---

# 28. Phase 2 — LLM Compile

명령:

```text
glyph synth
```

Graph Compile 이후 `unresolved` Node만 합성한다.

예:

```text
node CreateUser

in:
    UniqueRequest

out:
    User

ensure:
    User.id exists
    User.created_at exists

intent:
    신규 User를 생성한다.
```

LLM Compiler는 전체 프로젝트를 보내지 않는다.

---

# 29. Synthesis Unit

각 LLM 호출은 작은 독립 작업이다.

예:

```text
TARGET
CreateUser

INPUT
UniqueRequest

OUTPUT
User

AVAILABLE
System.NewId
System.Now

EFFECT
none

DOWNSTREAM
Save(User)

ENSURE
User.id exists
User.created_at exists

INTENT
신규 사용자를 생성한다.
```

LLM은 Node Implementation만 생성한다.

---

# 30. Graph Context Slicing

필요한 추가 문맥은 Program Graph에서 Compiler가 추출한다.

가능한 정보:

```text
Caller Contract
Callee Contract
Shared State
Relevant Types
Cross-node Invariants
Known Failures
Test Failures
Runtime Traces
Diagnostics
```

전체 repository context를 LLM에게 전달하지 않는다.

---

# 31. Program Knowledge Graph

Compiler는 다음 Entity를 관리한다.

```text
Flow
Node
Type
State
Effect
Failure
Invariant
Resource
Package
Test
Trace
Diagnostic
```

그리고 다음 Edge를 관리한다.

```text
CALL
DATA
READ
WRITE
FAIL
EFFECT
RESOURCE
DEPENDS
GUARANTEE
```

LLM RAG는 이 Graph를 우선 사용한다.

---

# 32. LLM Compiler의 역할

LLM Compiler가 하는 일:

```text
Contract
+
Intent
+
Relevant Graph Slice
        ↓
Node Implementation
```

LLM은 다음을 책임지지 않는다.

```text
전체 Architecture
Dependency Discovery
Memory Management
DB Connection
Resource Lifetime
Package Resolution
Global Graph
Type System
Build Scheduling
```

---

# 33. 기본 내장 모델

Glyph는 기본적으로 작은 코드 특화 모델을 내장할 수 있다.

가칭:

```text
GlyphCoder Tiny
```

목표 크기:

```text
0.5B ~ 2B
```

범용 코딩 모델일 필요가 없다.

오직:

```text
Node Contract
→ Node Implementation
```

작업에 특화한다.

---

# 34. Model Routing

합성 순서:

```text
Deterministic Synthesizer
        ↓ fail
GlyphCoder Tiny
        ↓ fail
GlyphCoder Small
        ↓ fail
External Model
```

쉬운 Node는 LLM 없이 Compiler가 직접 생성할 수 있다.

예:

```text
mapping
getter
simple reducer
type conversion
known DB pattern
known tensor pattern
```

---

# 35. 병렬 LLM Compile

Graph Compile이 끝난 시점에는 각 Node의 계약이 확정되어 있다.

따라서 독립 Node를 동시에 합성할 수 있다.

```text
Node A → Worker 1
Node B → Worker 2
Node C → Worker 3
Node D → Worker 4
```

Runtime dependency와 Synthesis dependency는 구분한다.

실행 순서가:

```text
A → B → C
```

여도 B가 A의 Contract만 알면 구현 가능하다면 A/B/C 합성은 동시에 수행할 수 있다.

---

# 36. Synthesis Dependency Graph

Compiler는 별도로 다음 그래프를 만든다.

```text
Runtime Dependency Graph
Synthesis Dependency Graph
```

이를 통해 가능한 한 많은 Node를 동시에 LLM API로 보낸다.

---

# 37. Verification

LLM 결과는 신뢰하지 않는다.

모든 Candidate는 다음 검사를 통과해야 한다.

```text
Parse
Type Check
Contract Check
Effect Check
State Access Check
Failure Check
Resource Check
Static Analysis
Node Test
Property Test
```

실패하면 Diagnostic만 다시 LLM에 전달한다.

---

# 38. Repair

예:

```text
REPAIR CreateUser

ERROR
expected User
found User?

FAILED CONTRACT
output.id exists
```

전체 프로젝트를 다시 전달하지 않는다.

---

# 39. Escalation

작은 모델이 여러 번 실패하면 더 큰 모델로 escalation한다.

```text
Tiny
 ↓ fail
Tiny Repair
 ↓ fail
Small
 ↓ fail
External Large Model
```

큰 모델은 정상 경로가 아니라 fallback이다.

---

# 40. LLM Compile 결과

LLM Compile 결과는 임시 cache가 아니라 코드다.

```text
Graph-Resolved Code
        +
Generated Node Implementations
        =
Fully-Resolved Glyph Code
```

이 결과를 Git에 저장하고 공유할 수 있다.

다른 개발자는 LLM 없이 build할 수 있다.

---

# 41. Source Determinism

LLM을 실행하는 명령만 코드를 변경할 수 있다.

```text
glyph synth
```

다음 명령은 source를 수정하지 않는다.

```text
glyph build
glyph run
glyph test
```

한번 합성된 코드가 자동으로 다시 생성되지 않는다.

---

# 42. Synthesis Lock

생성 기록은 `glyph.lock`에 저장할 수 있다.

예:

```text
CreateUser:
    spec_hash: 3A14
    context_hash: 9B32
    implementation_hash: C819
    generator: glyphcoder-tiny
```

Contract가 바뀌면:

```text
implementation = stale
```

이 된다.

---

# 43. Cache

Node synthesis cache key:

```text
hash(
    Node Contract
    Relevant Types
    Dependency Contracts
    Context Slice
    Compiler Version
    Synthesis Policy
)
```

동일하면 LLM 호출을 생략한다.

---

# 44. Incremental Synthesis

변경된 Type, Contract, State를 기준으로 영향 범위를 계산한다.

```text
Changed Contract
      ↓
Graph Impact Analysis
      ↓
Affected Nodes
      ↓
Only those Nodes synthesized
```

예:

```text
10000 Nodes
23 affected

→ 23 synthesis requests
```

---

# 45. Phase 3 — Executable Compile

명령:

```text
glyph build
```

입력:

```text
Fully-Resolved Glyph Code
```

출력:

```text
Executable
Library
WASM
```

Pipeline:

```text
Resolved Glyph
    ↓
Glyph IR
    ↓
Low-Level IR
    ↓
Optimization
    ↓
Native/WASM Backend
    ↓
Executable
```

이 단계에는 LLM이 없다.

---

# 46. 재현 가능한 빌드

LLM의 비결정성은 Phase 2에서 끝난다.

```text
Graph Compile
    deterministic

LLM Compile
    potentially nondeterministic

Executable Compile
    deterministic
```

Fully-Resolved Glyph Code를 공유하면 이후 build는 AI와 독립적이다.

---

# 47. 기본 Standard Node

v0.1 기본 Node 영역:

```text
Core
Text
Number
Collection
Time
Random

File
JSON
HTTP

State
DB

Crypto
Process

Tensor
NN
Train
Optim
Data

AI
```

---

# 48. LLM Runtime Node

컴파일 시 사용하는 LLM과 프로그램 실행 중 사용하는 LLM은 별개다.

Runtime LLM은 그냥 Node다.

```text
node AI.Classify

in:
    Text

out:
    Category

effect:
    ModelInference
```

특별한 실행 의미론을 갖지 않는다.

---

# 49. 보안

LLM에게 OS 전체 권한을 제공하지 않는다.

Synthesis Unit에 허용된 capability만 제공한다.

예:

```text
ALLOW
    Hash
    Compare
    System.Now

DENY
    Network
    File
    Process
```

생성 코드가 금지 capability를 사용하면 Compiler가 거부한다.

보안은 프롬프트가 아니라 언어 규약으로 강제한다.

---

# 50. 파일 구조

논리적으로 세 코드 상태가 존재한다.

```text
main.glyph
      ↓ Graph Compile

Graph-Resolved Glyph
      ↓ LLM Compile

Fully-Resolved Glyph
      ↓ Executable Compile

Binary
```

중간 representation을 실제 별도 파일로 노출할지는 구현 단계에서 결정한다.

---

# 51. CLI

v0.1 CLI:

```text
glyph graph
glyph synth
glyph check
glyph build
glyph run
glyph test
```

의미:

```text
glyph graph
    Graph Compile

glyph synth
    Graph Compile + LLM Compile

glyph check
    Contract / Graph / Type 검증

glyph build
    Executable Compile

glyph run
    build + execute

glyph test
    graph-aware tests
```

---

# 52. v0.1 최소 구현 범위

첫 구현에는 다음 기능만 있어도 된다.

```text
Primitive Type
Struct Type

Flow
Node

Input
Output

Intent
Ensure
Effect
Failure

Basic State
SQLite Backend

Graph Compiler
Dependency Graph
Context Slicer

Local LLM Backend
Remote LLM Backend

Node Verification
Synthesis Cache

C Runtime
C ABI

Native Build
```

---

# 53. 첫 Demo

첫 Demo는 간단한 API + DB 프로그램으로 한다.

```text
POST /user
     ↓
Parse
     ↓
Validate
     ↓
CheckDuplicate
     ↓
CreateUser
     ↓
User.Store
     ↓
Response
```

두 번째 Demo는 Neural Network 학습이다.

```text
Dataset
   ↓
Batch
   ↓
Linear
   ↓
GELU
   ↓
Linear
   ↓
CrossEntropy
   ↓
Backward
   ↓
AdamW
```

이 두 Demo를 모두 구현할 수 있다면 Glyph의 일반 애플리케이션 모델과 AI 계산 그래프 모델을 동시에 검증할 수 있다.

---

# 54. v0.1 주요 실험 지표

Glyph의 성공 여부는 코드 길이가 아니라 다음으로 측정한다.

```text
Tokens / Node
Tokens / Feature

Average Context Size
Context Size vs Project Size

Synthesis Success Rate
Repair Count

Tiny Model Success Rate
Large Model Escalation Rate

Cache Hit Rate
Incremental Recompile Count

Parallel Synthesis Speedup
Wall-clock Compile Time
```

특히 가장 중요한 실험은:

```text
Project Size ↑

LLM Context Size
≈ constant
```

가 실제로 가능한지를 확인하는 것이다.

---

# 55. 핵심 연구 가설

### H1

전체 repository를 이해하는 거대 모델은 필요하지 않을 수 있다.

Compiler가 전체 프로그램 구조를 관리하고 LLM은 Node 하나만 처리한다.

### H2

대부분의 Node synthesis는 작은 코드 특화 모델로 가능하다.

### H3

Graph 기반 Context Slicing으로 일반 바이브코딩보다 훨씬 적은 토큰으로 코드를 생성할 수 있다.

### H4

Node synthesis는 높은 수준으로 병렬화할 수 있다.

### H5

메모리, DB, dependency, resource, effect를 언어 자체가 관리하면 LLM이 해결해야 하는 문제 공간을 크게 줄일 수 있다.

---

# 56. Glyph의 책임 분리

사용자:

```text
무엇을 만들 것인가
어떤 절차인가
어떤 계약을 지켜야 하는가
어떤 의미를 원하는가
```

Compiler:

```text
프로그램 구조
타입
그래프
상태
메모리
DB
Effect
Resource
Dependency
Context
Validation
Scheduling
Cache
```

LLM:

```text
Node 내부의 구현
```

Runtime:

```text
실제 Node 실행
메모리
자원
DB backend
스케줄링
Native ABI
```

---

# 57. Glyph의 핵심 정의

Glyph는 자연어 프로그래밍 언어가 아니다.

Glyph는:

> **구조와 계약을 정형 코드로 작성하고, 세부 구현을 직접 작성하거나 자연어 명세를 통해 합성할 수 있는 Flow-oriented programming language다.**

자연어는 선택적인 구현 문법이다.

---

# 58. 핵심 문장

> **User defines the Flow.**

> **Compiler owns the Graph.**

> **LLM synthesizes the Node.**

> **Runtime executes the Program.**

그리고 Glyph의 가장 중요한 철학은 다음과 같다.

> **큰 모델이 필요한 문제를 푸는 대신, 문제를 작은 모델이 풀 수 있는 형태로 만든다.**

Glyph는 LLM에게 프로그램 전체를 이해시키지 않는다.

프로그램 전체는 Compiler가 이해한다.

LLM은 계약으로 격리된 작은 Node 하나만 구현한다.
