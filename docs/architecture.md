# 구현 결정과 후속 경로

이 문서는 구현 완료 목록을 과장하지 않기 위한 경계 문서다. v0.1 원문은 목표 명세이고, 현재 구현은 실행 가능한 compiler/runtime bootstrap이다.

## 세 단계의 실제 경계

1. Graph Compile: 소스 → 선언 AST → 타입·계약·effect 검사 → resolved Flow. 타입이 같은 값이 여럿이면 최근 값을 임의 선택하지 않고 모호성을 보고한다.
2. Synthesis: unresolved Node만 선택한다. 전체 저장소 대신 관련 타입의 전이적 closure와 해당 Node의 계약/intent를 입력으로 만든다. 결정적 구현 추출 또는 Glyph 모델 Forward를 실행한다. 결과는 제한된 Glyph 표현식이고 C++·Python·쉘 코드는 허용하지 않는다.
3. Executable Compile: resolved Flow와 Node 표현식을 versioned stack bytecode로 내린다. C++ VM이 이 artifact를 실행한다. 빌드에 LLM이나 C++ 코드 생성 단계는 없다.

Node는 현재 한 표현식으로 제한한다. 이것은 타입/효과 검사를 런타임 밖에서 우회하지 못하도록 하는 초기 언어 경계다. 향후 block 표현식이나 Node/Flow 호출을 추가할 때 동일한 IR 검증 경로를 유지해야 한다.

## 모델은 VM의 특별한 native 함수가 아니다

모델 계산 구조는 일반 `flow Forward`다. 컴파일러는 model source를 같은 frontend로 컴파일하고 runtime::Model에 Module을 넘긴다. `runtime::Model`은 토큰을 Tensor로 만들고 모듈을 반복 실행한다. Embedding/MatMul/Norm 등만 native primitive다. 현재 tokenizer와 생성 반복은 C++ 부트스트랩 계층이고, 모델의 계산 구조는 Glyph 계층이다.

현재 byte tokenizer는 엔진 통합을 검증하기 위한 독립 프로토콜이다. Qwen의 tokenizer나 architecture와 같다고 취급하면 안 된다. 테스트 모델은 학습 모델이 아닌 transition fixture다. Qwen 또는 fine-tuned GlyphCoder 품질을 평가하려면 별도 실제 모델 호환 작업이 필요하다.

## 메모리와 State

Value는 값 타입 또는 shared Tensor/record storage다. Flow IR의 LOAD 사용 횟수를 세어 마지막 사용 뒤 DROP을 삽입한다. alias를 가진 값은 참조가 남는 동안 살아 있으며 C++ VM stack/locals가 사라지면 RAII로 해제된다. 이 단계에서 GC/arena/buffer reuse를 구현했다고 주장하지 않는다.

SQLite 상태는 고정된 primitive schema를 가진 insert-only 모델이다. compiler는 write의 State→State signature, DB.Write, DBError 전파를 검사한다. VM은 parameter binding으로 데이터를 쓰고 atomic entry의 오류 시 rollback한다. 상태 schema 변경은 migration으로 해결하지 않으며 기존 DB와 맞지 않으면 SQLite 오류가 발생할 수 있다.

모든 Failure를 실제 typed error edge로 실행하는 구조는 아직 아니다. 현재는 정적으로 failure 선언을 전파시키고, VM 오류는 실패 상태와 진단으로 entry 밖에 반환한다. 계약 실패는 ContractViolation이며 fail 선언과 별도로 항상 실행을 중단한다.

## Qwen급 실제 모델까지 필요한 작업

1. 현재 Tensor 수치 kernel에 다양한 reference fixture와 오차 허용 기준을 추가한다. 다중 헤드 차원·reshape·slice를 갖는 Tensor IR과 subflow를 도입한다.
2. 선택한 **정확한 checkpoint**의 tokenizer, special tokens, weight metadata, shape를 검증하는 importer를 구현한다. 처음은 단순한 FP32/FP16으로 시작하고 safetensors 또는 다른 한 형식만 지원한다.
3. 모델 설정으로 Glyph graph를 만든다. Q/K/V projections, positional encoding, attention head 구성, gated FFN, final norm/LM head는 설정과 대응시킨다.
4. 짧은 고정 입력의 layer별 값과 logits를 reference와 비교한다. 출력 문장이 그럴듯한지만 보고 호환성을 인정하지 않는다.
5. cached/uncached logits 동등성 테스트를 전제로 KV resource를 도입한다. 이후 quantization, packed matrix, SIMD/Metal을 추가한다.
6. 실제 모델로 Node slice→표현식 데이터셋을 수집한다. 타입/효과 검증 성공과 계약/테스트 성공을 별도 라벨로 관리하고, 보류한 과제에서 합성 정확도와 context 크기를 측정한다.
7. fine-tuning은 모델 호환과 데이터 라벨 신뢰성이 확보된 뒤 수행한다. trace의 accepted는 현재 **구조적 검증 통과**이며 의미적 정답 라벨이 아니다.

## 검증 책임

- compiler tests: unknown/ambiguous symbols, 타입 오류, undeclared effects, unresolved build 금지, 소스 불변, 결정적인 artifact.
- runtime tests: 계약, Int overflow, SQLite atomic rollback, Tensor shape failure와 수치 결과.
- bootstrap model test: 외부 모델 런타임 없이 Forward → byte sampling → typed candidate → 실행.
- C ABI test: C 번역 단위에서 module load/run/free, 실패 진단과 문자열 ownership.

바이트코드는 compiler가 생성한 artifact를 위한 내부 형식이다. decoder에 기본 구조 검사와 VM stack/operand 검사가 있지만, 악의적인 bytecode를 실행하는 보안 sandbox나 완전한 load-time bytecode verifier로 제공하지 않는다.
