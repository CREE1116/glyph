# Glyph — C++ compiler & runtime bootstrap

Glyph 소스를 타입이 있는 Flow Graph로 검증하고, 바이트코드로 컴파일한 뒤 C++ VM에서 직접 실행하는 초기 구현입니다. **사용자 프로그램을 C++로 변환하거나 실행 시 C++ 컴파일러를 호출하지 않습니다.**

```text
.glyph → Parser → Type / Contract / Effect / Graph checks → .gyb → C++ VM
                      │
                      └─ unresolved Node → context slice
                                             ↓
                                      Glyph model Flow
                                             ↓
                                      같은 C++ VM / Tensor kernels
                                             ↓
                                      expression candidate
                                             ↓
                                      검증 → resolved.glyph
```

컴파일러, VM, CPU Tensor 커널, 모델 생성 드라이버 모두 C++17입니다. Python은 테스트에만 사용합니다. PyTorch, llama.cpp, Ollama, 외부 추론 서버는 사용하지 않습니다.

## 바로 실행

필요한 환경: C++17 컴파일러, Make, SQLite3 개발 라이브러리. macOS에서는 기본 개발 도구로 빌드했습니다.

```sh
make -j4
./build/glyph run examples/arithmetic.glyph -- 21
# "result=42"

./build/glyph build examples/arithmetic.glyph -o build/arithmetic.gyb
./build/glyph run build/arithmetic.gyb -- 21

./build/glyph run examples/tensor.glyph
# {"shape":[2,2],"data":[18,18,18,18]}

./build/glyph run examples/attention.glyph
./build/glyph run examples/multihead.glyph
./build/glyph run examples/register.glyph --db build/demo.sqlite -- user@example.com
make test
```

`register.glyph`은 SQLite 트랜잭션 예제입니다. ID를 1로 고정하므로 같은 DB에 두 번 실행하면 unique 위반으로 실패합니다. HTTP 서버는 포함하지 않습니다.

CMake도 지원합니다.

```sh
cmake -S . -B build/cmake
cmake --build build/cmake -j4
ctest --test-dir build/cmake --output-on-failure
```

## 키보드 입력 계산기

```sh
./build/glyph run examples/calculator.glyph
# 실행 후 첫 번째 정수와 두 번째 정수를 각각 입력: 기본 덧셈
./build/glyph run examples/calculator.glyph --flow Subtract
./build/glyph run examples/calculator.glyph --flow Multiply
./build/glyph run examples/calculator.glyph --flow Divide
```

`IO.readLine()`이 실행 중 표준 입력을 한 줄 읽고, `IO.print(Text)`가 표준 출력으로 한 줄을 출력합니다. 두 함수는 `effect: IO` 선언이 필요합니다. `Text.toInt(Text)`는 앞뒤 공백을 제거하고 Int64로 변환하며, 잘못된 숫자나 범위 초과는 오류로 보고합니다. 입력이 끝나면 `IOError`로 종료합니다.

계산기는 한 번에 정수 두 개를 계산합니다. 연산은 entry Flow로 선택하고, 마지막 Flow 반환값은 CLI가 출력합니다. 나눗셈은 소수점 이하를 버리는 정수 나눗셈이며 0으로 나누면 계약 오류가 발생합니다. 반복 입력이나 오류 후 재입력은 아직 지원하지 않습니다.

## 언어 예제

```glyph
node Double
in:
    value: Int > 0
out:
    Int
ensure:
    output == value * 2
impl:
    value * 2

flow Main
in:
    number: Int
out:
    Int
Double
```

이름 없는 `in: Int`는 `input`으로 참조합니다. Flow에서 해당 타입의 앞선 값이 하나면 자동 연결합니다. 두 개 이상이면 컴파일 오류이며 `Double(number) -> doubled`처럼 명시합니다. 기존 값을 새 값으로 덮어쓰지 않습니다. Flow의 마지막 Node 출력이 Flow 출력입니다.

구조체는 `type User {` 다음 줄에 `id: Int` 같은 필드를 선언하고 `User(1, "name")`처럼 생성합니다. 필드는 `output.id`로 접근합니다. Node 구현은 현재 한 개의 타입 검사 가능한 표현식입니다. `intent:`는 실행 코드가 아닙니다.

## 명령

| 명령 | 실제 동작 |
|---|---|
| `glyph check source.glyph` | 전체 소스의 타입·계약 표현식·효과·Flow 연결 검사. unresolved 개수 표시 |
| `glyph graph source.glyph` | 연결된 입력 바이트코드, Node 순서, 효과, unresolved를 JSON으로 출력 |
| `glyph build source.glyph -o app.gyb` | 결정적인 버전 1 바이트코드 생성. unresolved가 있으면 실패 |
| `glyph run source.glyph 또는 app.gyb -- 인수` | 컴파일 후 VM 실행. 반환값은 JSON으로 출력 |
| `glyph run source.glyph --model MODEL_DIR -- 인수` | 미해결 Node를 메모리에서 합성한 뒤 실행. 소스는 그대로 |
| `glyph test source.glyph -- 인수` | 한 번의 entry 실행 및 실행된 계약 검사. 테스트 자동 생성 기능은 아님 |
| `glyph synth source.glyph --units` | unresolved Node별 관련 타입·계약·intent slice 출력 |
| `glyph synth source.glyph -o resolved.glyph` | 결정적 합성 우선, 필요 시 `--model`의 Glyph 모델 실행 |

Flow가 여러 개면 `--flow Name`으로 entry를 선택합니다. 기본값은 `Main` 또는 유일한 Flow입니다. 바이트코드는 빌드 시 entry가 고정됩니다. CLI 입력은 Int/Float/Bool/Text를 지원합니다. Tensor·구조체 입력은 내부 Flow에서 구성하거나 C++ 런타임 API로 전달합니다.

`build`, `run`, `check`, `test`는 소스를 변경하지 않습니다. `synth`도 별도의 `-o` 파일에 저장하며 원본 덮어쓰기를 거부합니다.

## 내장 그래프 시각화

```sh
./build/glyph graph examples/attention.glyph --html -o build/attention.html
```

생성된 HTML을 브라우저에서 열면 됩니다. 화면 코드와 그래프 데이터가 파일 하나에 들어 있으며 서버, 인터넷, CDN, 추가 패키지가 필요 없습니다.

- Flow 선택, Node/binding 검색, 확대·축소.
- 실선은 실제 데이터 의존성, 점선은 실행 순서. 인수 표현식에서 참조하는 binding을 컴파일러가 추출합니다.
- Node 선택 시 입력·출력 타입, 선언/호출 줄 번호, 계약, 효과, State 접근, failure 선언, intent, 구현을 표시합니다.
- unresolved Node는 주황 점선 테두리로 표시합니다. 같은 Node를 여러 번 호출해도 각각의 호출과 binding을 구분합니다.
- 기존 `glyph graph` JSON 출력도 유지하며 Node 메타데이터와 명시적 `edges`가 추가되었습니다.

현재는 **컴파일 시점의 읽기 전용 스냅샷**입니다. 그래프 편집, 실행 중 추적, 자동 새로고침은 지원하지 않습니다. 효과와 failure는 계약 정보로 표시하며 별도의 실행 가능한 handler graph를 만들어내지 않습니다.

## 합성과 모델 런타임

모델 없이 재현 가능한 예제:

`examples/intent.glyph`는 impl이 없는 Node 두 개로 된 파이프라인입니다. 각 Node는 `intent`로 할 일을, `ensure`로 지켜야 할 것을 선언하고 구현은 모델이 씁니다.

`run`, `test`, `build`에 `--model`을 주면 미해결 Node를 그 자리에서 합성한 뒤 이어서 실행합니다. 소스 파일은 건드리지 않고 해결된 프로그램은 그 프로세스 안에만 존재합니다.

```sh
./build/glyph run examples/intent.glyph --model path/to/Qwen2.5-0.5B-Instruct -- -20 64
# 64
```

생성된 소스를 남기려면 `synth`로 파일에 쓰고 그 파일을 실행합니다.

```sh
./build/glyph synth examples/intent.glyph -o build/intent.glyph \
    --model path/to/Qwen2.5-0.5B-Instruct
./build/glyph run build/intent.glyph -- -20 64
# 64
```

Qwen2.5-0.5B-Instruct로 확인했을 때 생성된 구현은 `select(raw >= 0, raw, 0)`과 `select(first > second, first, second)`이고, 입력 `-20 64`, `80 64`, `5 5`, `-3 -9`에 대해 각각 64, 80, 5, 0을 반환합니다.

```sh
./build/glyph synth examples/synthesize.glyph \
    -o build/resolved.glyph --trace build/synthesis.jsonl
./build/glyph run build/resolved.glyph -- 12
# 24
```

`ensure: output == 표현식`에서 구현을 결정적으로 추출할 수 있습니다. 나머지는 사용자가 제공한 모델 Flow로 처리합니다.

### 하네스: 컴파일러가 결정할 수 있는 것은 모델에 넘기지 않습니다

모델 후보를 판정하기 전에 결정적 정규화를 한 번 거칩니다. 벤치마크에서 실패 대부분이 추론 실패가 아니라 형식 오류였고, 그 형식은 컴파일러가 이미 알고 있는 것들입니다.

| 후보 | 정규화 결과 | 근거 |
|---|---|---|
| 코드펜스로 감싼 여러 줄 | 펜스 제거 후 첫 줄 | 형식 정리 |
| `True`, `False` | `true`, `false` | 리터럴 표기는 렉서가 안다 |
| `max(a, b)`, `min(a, b)`, `abs(x)` | 동등한 `select` 식 | 치환 규칙이 하나뿐이다 |
| 미바인딩 이름 하나, 입력이 하나 | 그 입력 이름으로 치환 | 다른 뜻일 수가 없다 |

모호하면 적용하지 않습니다. 입력이 둘인데 미바인딩 이름이 둘이면 대응이 유일하지 않으므로 그대로 두고 타입 검사가 거부합니다. 정규화한 식도 타입·효과·계약 검증을 그대로 통과해야 하며, 적용한 치환은 stderr와 trace에 남습니다.

같은 이유로 두 가지를 더 컴파일러 쪽으로 옮겼습니다. 진단은 계약을 깬 노드를 정확히 지목합니다. 그리고 greedy 디코딩에서 같은 후보가 다시 나오면 repair가 아무것도 바꾸지 못했다는 뜻이므로 재시도를 그 자리에서 멈춥니다.

후보는 두 단계로 검증합니다. 먼저 일반 Node와 같은 parser/type/effect 검사를 통과해야 합니다. 그다음 그 Node만 담은 임시 1노드 Flow를 만들어 입력 격자 위에서 실제로 실행합니다. 프로그램의 실제 Flow로 검사하면 형제 Node가 아직 미해결일 때 검사를 건너뛰게 되고, 이웃이 깬 계약을 엉뚱한 Node 탓으로 돌립니다. 실행 중에 `ensure` 계약이 깨지거나 0 나눗셈 같은 산술 오류가 나면 후보를 반례 입력과 함께 거부하고, 그 진단을 repair 프롬프트에 넣습니다. 입력 refinement나 `require`를 벗어난 격자 점은 Node가 처리하겠다고 약속한 적이 없는 입력이므로 건너뜁니다. 격자는 타입별 고정 값이며 조합 수를 64개로 제한합니다. 이것은 계약 증명이 아니라 반례 탐색입니다.

### 합성 벤치마크

`bench/synthesis_bench.py`는 "모델이 자연어 intent를 읽고 실제로 동작하는 코드를 내는가"를 측정합니다. 12개 과제가 각각 intent와, 답을 알려주지 않는 범위의 `ensure` 계약을 선언합니다. `output == 표현식` 형태는 하나도 쓰지 않으므로 결정적 추출로는 풀 수 없습니다.

과제마다 세 단계를 기록합니다.

- `compiled`: parser/type/effect 검사 통과.
- `accepted`: 계약 실행 검증까지 통과해 소스가 실제로 생성됨.
- `correct`: 생성된 소스를 참조 구현과 비교해 일치. 비교에 쓰는 입력 격자는 계약 검증에 쓰는 격자와 일부러 다르게 두어 격자에 과적합된 답을 잡습니다.

```sh
GLYPH_BIN=build/glyph python3 bench/synthesis_bench.py path/to/Qwen2.5-0.5B-Instruct
```

greedy 디코딩이므로 재실행해도 같은 값이 나옵니다.

| 지표 | Qwen2.5-0.5B-Instruct | Qwen2.5-Coder-0.5B-Instruct |
|---|---|---|
| 계약 검증까지 통과 | 7 | 4 |
| 참조 구현과 일치 | 6 | 1 |
| 통과했지만 틀림 | 1 | 3 |

같은 크기의 Coder 변종이 오히려 나쁩니다. Coder는 프롬프트 예시의 변수 이름 `x`를 그대로 베끼고, 정규화가 입력이 하나인 Node에서만 그것을 복구할 수 있습니다. 코드 특화가 이 과제에서 도움이 되려면 더 큰 Coder 모델이 필요해 보입니다.

아래 해석은 Instruct 쪽 숫자입니다. 모델이 틀린 답을 낸 6과제 중 5과제는 컴파일러와 계약이 막아서 소스가 생성되지 않았습니다. 즉 결합의 효과는 "틀린 코드"를 "코드 없음"으로 바꾸는 것입니다. 통과했지만 틀린 1과제는 Sign이며, `output >= -1`과 `output <= 1`만 선언해서 `select(value > 0, 1, -1)`이 계약을 만족해 버립니다. 계약이 약하면 검증도 약하다는 뜻이고, 도구의 버그가 아닙니다.

0.5B가 성공한 과제는 select 한 번으로 끝나는 것들입니다(절댓값, 하한 0, 두 값 중 큰 값·작은 값, 차이, 짝수 판정). 실패는 두 종류로 갈립니다. 중첩 select가 필요한 과제(0~100 clamp, 학점 변환)와 출력이 `Bool`인 과제 일부에서 타입이 맞지 않는 식을 반복해서 냈습니다. 모델 크기 문제로 보이며, 더 큰 모델이나 코더 모델로 같은 벤치마크를 돌리면 바로 비교됩니다.

```sh
./build/glyph synth application.glyph -o build/resolved.glyph \
    --model path/to/model.glyph --trace build/training-candidates.jsonl
```

모델의 인터페이스는 `flow Forward`, 입력 `Tensor`, 출력 `Tensor`입니다. 모델 flow가 선언할 수 있는 효과는 가중치를 읽는 `File.Read`와 KV cache를 쓰는 `Model.Cache`뿐입니다. 모델 소스도 같은 컴파일러가 검사하고 같은 바이트코드로 내립니다. `runtime/model.cpp`는 byte tokenizer와 greedy 생성 루프만 담당하며, **모델의 forward 구조는 Glyph 코드에 있습니다.** 모델이 반환한 구현은 일반 Node와 같은 parser/type/effect 검사로 재검증합니다. 실패 진단을 포함한 재시도는 최대 3회입니다.

초기 모델 프로토콜:

- 입력: `[sequence_length, 1]` Tensor, UTF-8 바이트 ID 0–255.
- 출력: `[N, 257]` logits. 마지막 행의 argmax를 다음 토큰으로 사용, 256은 EOS.
- 전체 context 4096바이트, 생성 최대 256토큰.
- 가중치: `Tensor.load("weights.tensor")`; 파일 첫 줄 `GLYPH-TENSOR-1 rows cols`, 이후 행 우선 Float 값.
- 모델에서 허용하는 부작용은 가중치 파일을 읽는 `File.Read`뿐입니다. 상대 경로는 실행 디렉터리 기준입니다.
- `--trace`는 Node slice, candidate, 구조적 검증 결과와 실패 진단을 JSONL로 저장합니다. fine-tuning 수집의 시작점이며 학습 파이프라인은 아닙니다.

테스트의 생성 모델은 `input`이라는 표현식을 내는 결정적 transition fixture입니다. 이것은 `Glyph 모델 실행 → 합성 → 컴파일 → 실행` 연결을 검증하며, LLM 성능을 검증하지 않습니다. `examples/attention.glyph`는 고정 가중치의 단일 헤드 Attention 수치 예제이고, `examples/multihead.glyph`는 같은 방식으로 2개 query head가 1개 key/value head를 공유하는 GQA 블록을 계산합니다. 두 예제 모두 checkpoint 없이 실행됩니다.

### Qwen2 가중치 직접 로드

`--model`에 파일 대신 디렉터리를 주면 dense Qwen2/Qwen2.5 checkpoint를 직접 읽습니다. 외부 추론 엔진은 사용하지 않습니다. 지원 조건은 `model_type: qwen2`, SiLU, 기본 RoPE, full attention이며 sliding window와 rope scaling은 거부합니다.

- 가중치: safetensors를 직접 파싱합니다. F32/BF16/F16을 FP32로 변환해 읽고 nonfinite 값을 거부합니다. 샤딩된 checkpoint는 `model.safetensors.index.json`을 따릅니다.
- 토크나이저: `tokenizer.json`의 BPE vocab/merges와 byte-level 매핑을 직접 구현합니다. pre-tokenizer 분할은 PCRE2의 UTF/UCP로 Qwen2 정규식을 그대로 사용하고 `added_tokens`를 special token으로 먼저 끊습니다.
- forward: importer가 checkpoint를 읽어 **일반 Glyph 소스**를 생성하고, 같은 컴파일러와 VM이 실행합니다. `runtime/weights.cpp`는 primitive만 제공합니다. multi-head RoPE는 `Tensor.MultiRoPE`, GQA causal attention은 `Tensor.Attend`입니다.
- 행렬 연산은 FP32이며 Linear는 BLAS `sgemm`을 사용합니다. context는 2048 토큰으로 제한합니다.
- KV cache는 attention 노드가 이름 있는 slot을 소유하는 방식으로 구현했습니다. 노드가 `effect: Model.Cache`를 선언하고 `Tensor.AttendCached(q, k, v, heads, kv, "layer.0")`로 slot을 명시하므로 숨은 상태가 아닙니다. 위치 오프셋은 `flow Forward`의 두 번째 입력 `offset: Int`으로 들어가고 `Tensor.MultiRoPEAt`이 그 값을 씁니다.
- 프롬프트 전체를 offset 0으로 한 번에 넣는 경로와 토큰 하나씩 넣는 경로가 같은 그래프를 씁니다. cache가 비어 있고 offset이 0이면 계산은 전체 재계산과 동일하므로 검증 경로가 그대로 유효합니다.

```sh
./build/glyph model path/to/Qwen2.5-0.5B-Instruct --tokens "Hello world"
./build/glyph model path/to/Qwen2.5-0.5B-Instruct --logits "The capital of France is"
./build/glyph model path/to/Qwen2.5-0.5B-Instruct --generate "The capital of France is"
./build/glyph model path/to/Qwen2.5-0.5B-Instruct --cache-check "The capital of France is"
./build/glyph model path/to/Qwen2.5-0.5B-Instruct --export build/qwen.glyph
./build/glyph synth application.glyph -o build/resolved.glyph \
    --model path/to/Qwen2.5-0.5B-Instruct
```

`--tokens`는 토크나이저 ID, `--logits`는 마지막 위치 logits, `--generate`는 greedy로 16토큰 이어쓰기 ID를 출력합니다. `--cache-check`는 cache로 한 스텝씩 디코딩한 logits와 같은 prefix를 처음부터 다시 계산한 logits의 최대 차이를 스텝별로 보여줍니다. `--export`는 실행하지 않고 생성된 Glyph forward 그래프만 파일로 씁니다.

#### 출력 검증

`tests/test_model.py`는 같은 checkpoint에 대해 Hugging Face transformers FP32 출력과 비교합니다. 토크나이저 ID 전체와 마지막 위치 logits의 argmax가 일치해야 하고 최대 절대 오차 허용치는 5e-3입니다. 모델 디렉터리나 참조 스택이 없으면 테스트는 스스로 skip합니다.

```sh
python3 -m pip install torch transformers
GLYPH_BIN=build/glyph GLYPH_QWEN_DIR=path/to/Qwen2.5-0.5B-Instruct \
    python3 tests/test_model.py
```

확인하는 항목은 세 가지입니다.

1. 토크나이저 ID 전체 일치.
2. 마지막 위치 logits의 argmax 일치와 절대 오차.
3. greedy 16토큰 이어쓰기 ID 일치. 참조 쪽은 checkpoint의 sampling 기본값(`do_sample`, `repetition_penalty` 1.1)을 끄고 plain greedy로 맞춰야 같은 결과가 나옵니다.

여기에 `--cache-check` 결과로 cache 경로와 전체 재계산 경로의 차이도 검사합니다.

Qwen2.5-0.5B-Instruct에서 영어, 한국어, 코드, 이모지, chat template, 89토큰 시퀀스 7개 프롬프트가 모두 일치했습니다.

| 항목 | 관측값 |
|---|---|
| logits 최대 절대 오차 | 1.3e-4 |
| cache 경로와 전체 재계산의 최대 차이 | 9.4e-5 |
| greedy 16토큰 ID 일치 | 4/4 프롬프트 |

오차는 FP32 누적 순서 차이 범위입니다.

#### 속도

0.5B에서 weight를 읽고 그래프를 컴파일하는 고정 비용이 약 1.1초입니다. 그 다음 생성 속도는 cache 도입으로 다음과 같이 바뀌었습니다.

| 측정 | cache 없음 | cache |
|---|---|---|
| 토큰당 생성 시간 (prefix 약 300토큰) | 0.67s | 0.037s |
| 합성 후보 1개 생성 | 10.1s | 1.5s |

## 구현 범위와 경계

| 영역 | 구현 | 아직 구현하지 않은 부분 |
|---|---|---|
| 타입 | Int64, Float64, Bool, Text, Unit, 값 구조체, Tensor | generics, optional, list, union, 정적 Tensor shape |
| 그래프 | 선형 Flow, 타입 기반 자동 연결, 명시적 인수, immutable binding | 중첩 Flow 호출, 분기/반복 문법, 병렬 스케줄러 |
| 계약 | 입력 refinement, `require`, `ensure`의 순수 Bool 검사 및 실행 검사 | 정리 증명, 자연어 ensure, property test 생성 |
| 효과 | IO, File.Read, DB.Write, Model.Cache 선언과 실제 builtin 사용 비교 | OS 수준 sandbox, 모든 effect backend |
| 오류 | Node failure를 Flow에 명시적으로 전파하도록 검사 | failure별 handler edge, retry, compensation |
| State | SQLite insert, unique, persistent/temporary, atomic rollback | 조회/where, update, migration, PostgreSQL |
| 메모리 | RAII/shared ownership, Flow 값 마지막 사용 후 DROP | arena/추적 GC, Tensor buffer pool, SIMD/GPU |
| Tensor | MatMul/Add/Mul/Transpose/Softmax/RMSNorm/RoPE/SiLU/Embedding/CausalMask/Scale/Last/values, multi-head RoPE, GQA causal attention, slot 기반 KV cache, FP32 BLAS Linear | autodiff/optimizer, quantization, sliding window, batch 차원 |
| 합성 | Node context slice, 결정적 합성, Glyph 모델 추론, Qwen2 BPE·safetensors import, 타입 검증, ensure 실행 검증, repair, JSONL | 학습, candidate cache/lock, model routing, sampling 옵션 |
| 배포 | `.gyb`, C++ VM, C ABI 정적 라이브러리 | AOT machine code, standalone app binary, WASM |

`select(Bool, T, T)`와 논리 연산자는 현재 양쪽 피연산자를 평가합니다. Text 길이는 UTF-8 바이트 수입니다. Int 연산은 overflow와 0 나눗셈을 감지합니다. Tensor는 CPU Float64 참조 구현이며 최대 16M 원소로 제한합니다. `check`의 성공은 자연어 의미나 모든 입력에 대한 계약 증명을 뜻하지 않습니다. 컴파일 시 계약 표현식을 검사하고 실제 조건은 실행 시 검사합니다.

## 코드 구조

- `src/parser.cpp`: 소스/표현식 외 선언 parser와 진단.
- `src/expression.cpp`: 표현식 lexer, precedence parser, 타입·효과 검사, stack IR.
- `src/compiler.cpp`: symbol/type/graph resolution, 계약 lowering, last-use release, bytecode emission, context slicing.
- `src/graph.cpp`, `src/graph_view.hpp`: 그래프 JSON export와 독립형 HTML 시각화.
- `src/synth.cpp`: 합성 순서, 모델 graph compilation, candidate 타입 검증과 계약 실행 검증, resolved source와 trace 생성.
- `runtime/kernel.cpp`: bytecode decoder, VM, 계약/오류, SQLite transaction.
- `runtime/tensor.cpp`: CPU Tensor primitive.
- `runtime/model.cpp`: 같은 VM을 호출하는 byte-level autoregressive 생성 드라이버.
- `runtime/weights.cpp`: safetensors 파서와 FP32 weight primitive(Linear/Embed/Bias/Norm/MultiRoPE/GQA).
- `runtime/tokenizer.cpp`: Qwen2 byte-level BPE 인코더와 디코더.
- `runtime/qwen.cpp`: checkpoint를 Glyph forward 소스로 바꾸는 importer와 greedy 생성 루프.
- `runtime/abi.cpp`, `include/glyph/runtime.h`: opaque module C ABI. `make`가 `build/libglyph.a`도 생성합니다.

초기 설계 원문은 `docs/spec-v0.1.md`, 추가 모델 방향은 `docs/model-runtime-vision.md`, 후속 구현 순서는 `docs/architecture.md`에 보관했습니다.
