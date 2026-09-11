"""Black-box compiler/VM acceptance and negative tests. No third-party modules."""
import json
import math
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BIN = Path(os.environ.get('GLYPH_BIN', ROOT / 'build/glyph'))

class GlyphTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
    def tearDown(self):
        self.tmp.cleanup()
    def cli(self, *args, ok=True, input=None):
        p = subprocess.run([str(BIN), *map(str, args)], cwd=ROOT, text=True, capture_output=True, input=input)
        if ok:
            self.assertEqual(p.returncode, 0, p.stderr)
        else:
            self.assertNotEqual(p.returncode, 0, p.stdout)
        return p
    def source(self, text):
        p = self.dir / 'main.glyph'
        p.write_text(text)
        return p
    def program(self, expr, out='Int', extra=''):
        return self.source(f'node N\nout:\n    {out}\n{extra}impl:\n    {expr}\n\nflow Main\nout:\n    {out}\nN\n')
    def test_native_bytecode_and_source_unchanged(self):
        source = ROOT/'examples/arithmetic.glyph'
        before = source.read_bytes()
        artifact = self.dir/'app.gyb'
        self.cli('build', source, '-o', artifact)
        self.assertTrue(artifact.read_text().startswith('GLYPH-BC 1\n'))
        self.assertEqual(json.loads(self.cli('run', artifact, '--', '21').stdout), 'result=42')
        second = self.dir/'second.gyb'
        self.cli('build', source, '-o', second)
        self.assertEqual(artifact.read_bytes(), second.read_bytes())
        self.assertEqual(source.read_bytes(), before)
    def test_contract_failure(self):
        p = self.cli('run', ROOT/'examples/arithmetic.glyph', '--', '0', ok=False)
        self.assertIn('ContractViolation', p.stderr)
    def test_postcondition_failure(self):
        p = self.program('1', extra='ensure:\n    output > 2\n')
        self.assertIn('ContractViolation', self.cli('run', p, ok=False).stderr)
    def test_type_error(self):
        self.assertIn('returns Text, expected Int', self.cli('check', self.program('"x"'), ok=False).stderr)
    def test_effect_violation(self):
        self.assertIn('undeclared effect IO', self.cli('check', self.program('IO.print("x")','Text'), ok=False).stderr)
    def test_contract_is_pure(self):
        p = self.program('"x"','Text','ensure:\n    IO.print(output) == "x"\n')
        self.assertIn('pure Bool', self.cli('check',p,ok=False).stderr)
    def test_overflow_and_zero_division(self):
        for expr, error in [('9223372036854775807 + 1','overflow'), ('3 / 0','division by zero')]:
            with self.subTest(expr=expr):
                self.assertIn(error, self.cli('run',self.program(expr),ok=False).stderr)
    def test_string_roundtrip(self):
        p = self.program(r'"한글\n\"quoted\"\\path"', 'Text')
        self.assertEqual(json.loads(self.cli('run',p).stdout),'한글\n"quoted"\\path')
    def test_ambiguous_wiring(self):
        p = self.source('node N\nin:\n    Int\nout:\n    Int\nimpl:\n    input\nflow Main\nin:\n    a: Int\n    b: Int\nout:\n    Int\nN\n')
        self.assertIn('ambiguous input',self.cli('check',p,ok=False).stderr)
    def test_immutable_binding(self):
        p = self.source('node N\nout:\n    Int\nimpl:\n    1\nflow Main\nout:\n    Int\nN() -> x\nN() -> x\n')
        self.assertIn('reassignment',self.cli('check',p,ok=False).stderr)
    def test_unhandled_failure(self):
        p = self.program('1', extra='fail:\n    NotFound\n')
        self.assertIn('unhandled failure NotFound',self.cli('check',p,ok=False).stderr)
    def test_unresolved_and_deterministic_synthesis(self):
        p = ROOT/'examples/synthesize.glyph'
        graph = json.loads(self.cli('graph',p).stdout)
        self.assertEqual(graph['unresolved'],['Double'])
        self.cli('build',p,'-o',self.dir/'app.gyb',ok=False)
        result, trace = self.dir/'resolved.glyph',self.dir/'trace.jsonl'
        self.cli('synth',p,'-o',result,'--trace',trace)
        self.assertEqual(self.cli('run',result,'--','11').stdout.strip(),'22')
        self.assertTrue(json.loads(trace.read_text())['accepted'])
    def test_synthesis_slices_exclude_unrelated_intent(self):
        p = self.source('node N\nout:\n    Int\nintent:\n    target\nnode Other\nout:\n    Int\nintent:\n    UNRELATED_SECRET\nimpl:\n    0\nflow Main\nout:\n    Int\nN\n')
        self.assertNotIn('UNRELATED_SECRET',self.cli('synth',p,'--units').stdout)
    def test_sqlite_unique_and_rollback(self):
        source = (ROOT/'examples/register.glyph').read_text()
        source = source.replace('SaveUser(user)', 'SaveUser(user) -> saved\nSaveUser(user)')
        p = self.source(source)
        db = self.dir/'users.sqlite'
        self.assertIn('DBError', self.cli('run',p,'--db',db,'--','test@example.com',ok=False).stderr)
        with sqlite3.connect(db) as conn:
            self.assertEqual(conn.execute('select count(*) from User').fetchone()[0],0)
        self.cli('run',ROOT/'examples/register.glyph','--db',db,'--','test@example.com')
        with sqlite3.connect(db) as conn:
            self.assertEqual(conn.execute('select email from User').fetchone()[0],'test@example.com')
    def test_tensor_matmul(self):
        got = json.loads(self.cli('run',ROOT/'examples/tensor.glyph').stdout)
        self.assertEqual(got, {'shape':[2,2], 'data':[18,18,18,18]})
    def test_attention_graph(self):
        got = json.loads(self.cli('run',ROOT/'examples/attention.glyph').stdout)
        self.assertEqual(got['shape'],[2,4])
        for x in got['data']:
            self.assertAlmostEqual(x, 1.0)
    def test_tensor_values_literal(self):
        p = self.program('Tensor.values(2, 3, "1 2 3 4.5 -5 6")','Tensor')
        self.assertEqual(json.loads(self.cli('run',p).stdout),
                         {'shape':[2,3], 'data':[1,2,3,4.5,-5,6]})
    def test_tensor_values_count_checked(self):
        short = self.program('Tensor.values(2, 2, "1 2 3")','Tensor')
        self.assertIn('rows*cols',self.cli('run',short,ok=False).stderr)
        long = self.program('Tensor.values(2, 2, "1 2 3 4 5")','Tensor')
        self.assertIn('more numbers',self.cli('run',long,ok=False).stderr)
    def test_grouped_query_attention_graph(self):
        # Cross-checked against a NumPy/PyTorch reference; see tests/test_model.py
        # for the full-model FP32 parity test.
        got = json.loads(self.cli('run',ROOT/'examples/multihead.glyph').stdout)
        self.assertEqual(got['shape'],[1,8])
        expected = [-0.22371497, 0.66933794, -0.04917657, 0.71880457,
                    0.03721908, 0.94369819, -0.21400495, 0.57491135]
        for value, reference in zip(got['data'], expected):
            self.assertAlmostEqual(value, reference, places=6)
    def test_multihead_rope_rejects_bad_head_split(self):
        p = self.program('Tensor.MultiRoPE(Tensor.full(2, 6, 1.0), 4, 10000.0)','Tensor')
        self.assertIn('multi-head RoPE',self.cli('run',p,ok=False).stderr)
    def test_attend_rejects_group_mismatch(self):
        # 3 query heads cannot be split across 2 key/value heads.
        p = self.program('Tensor.Attend(Tensor.full(2, 12, 1.0), Tensor.full(2, 8, 1.0), '
                         'Tensor.full(2, 8, 1.0), 3, 2)','Tensor')
        self.assertIn('GQA shape mismatch',self.cli('run',p,ok=False).stderr)
    def test_attend_is_causal(self):
        # Row 0 may only read value row 0, so it equals that row exactly.
        p = self.program('Tensor.Attend(Tensor.full(2, 2, 1.0), Tensor.full(2, 2, 1.0), '
                         'Tensor.values(2, 2, "5 7 -1 -3"), 1, 1)','Tensor')
        got = json.loads(self.cli('run',p).stdout)
        self.assertAlmostEqual(got['data'][0], 5.0)
        self.assertAlmostEqual(got['data'][1], 7.0)
    def test_tensor_shape_failure(self):
        p = self.program('Tensor.MatMul(Tensor.full(2, 3, 1.0), Tensor.full(2, 2, 1.0))','Tensor')
        self.assertIn('shape mismatch',self.cli('run',p,ok=False).stderr)
    def test_softmax_stable(self):
        p = self.program('Tensor.Softmax(Tensor.full(1, 3, 1000.0))','Tensor')
        for x in json.loads(self.cli('run',p).stdout)['data']:
            self.assertAlmostEqual(x,1/3)
    def transition_model(self):
        # Deliberately deterministic transition fixture, NOT a pretrained LLM.
        # Prompt ends with newline; model emits i n p u t EOS.
        weights = self.dir/'transition.tensor'
        data = [[-10.0]*257 for _ in range(257)]
        for row in data:
            row[256] = 0.0
        for a,b in zip([10,105,110,112,117,116],[105,110,112,117,116,256]):
            data[a][b] = 10.0
        weights.write_text('GLYPH-TENSOR-1 257 257\n'+'\n'.join(' '.join(map(str,r)) for r in data))
        model = self.dir/'model.glyph'
        model.write_text(f'node ForwardNode\nin:\n    tokens: Tensor\nout:\n    Tensor\neffect:\n    File.Read\nimpl:\n    Tensor.Last(Tensor.Embedding(Tensor.load({json.dumps(str(weights))}), tokens))\nflow Forward\nin:\n    tokens: Tensor\nout:\n    Tensor\nForwardNode(tokens)\n')
        return model
    def test_ensure_contract_rejects_candidate(self):
        # The fixture answers 'input'. It types and compiles, but violates the
        # declared ensure, so synthesis must run the contract and refuse it.
        model = self.transition_model()
        # Not an `output == expression` contract, so deterministic extraction
        # cannot answer it and the model fixture is consulted.
        p = self.source('node Bigger\nin:\n    Int\nout:\n    Int\nensure:\n    output > input\nintent:\n    Return a number larger than the input.\nflow Main\nin:\n    Int\nout:\n    Int\nBigger\n')
        out,trace = self.dir/'rejected.glyph', self.dir/'trace.jsonl'
        stderr = self.cli('synth',p,'-o',out,'--model',model,'--trace',trace,ok=False).stderr
        self.assertIn('ContractViolation',stderr)
        self.assertIn('Bigger: ensure',stderr)
        self.assertFalse(out.exists())
        records = [json.loads(line) for line in trace.read_text().splitlines()]
        self.assertTrue(records)
        for record in records:
            self.assertFalse(record['accepted'])
            self.assertEqual(record['candidate'],'input')
            self.assertIn('input = ',record['diagnostic'])
    def test_model_runs_in_glyph_runtime(self):
        model = self.transition_model()
        p = self.source('node Identity\nin:\n    Int\nout:\n    Int\nintent:\n    Return input unchanged.\nflow Main\nin:\n    Int\nout:\n    Int\nIdentity\n')
        out,trace=self.dir/'resolved.glyph',self.dir/'trace.jsonl'
        self.cli('synth',p,'-o',out,'--model',model,'--trace',trace)
        self.assertEqual(self.cli('run',out,'--','42').stdout.strip(),'42')
        record=json.loads(trace.read_text())
        self.assertEqual(record['generator'],'glyph.graph.byte-lm.v1')
        self.assertEqual(record['candidate'],'input')
        # Invalid model candidates must be repaired/rejected, never emitted as source.
        bad = self.source('node Bad\nin:\n    Int\nout:\n    Text\nintent:\n    Convert to text.\nflow Main\nin:\n    Int\nout:\n    Text\nBad\n')
        rejected = self.dir/'rejected.glyph'
        self.cli('synth',bad,'-o',rejected,'--model',model,'--trace',trace,ok=False)
        self.assertFalse(rejected.exists())
        attempts = [json.loads(line) for line in trace.read_text().splitlines()]
        self.assertEqual(len(attempts),3)
        self.assertTrue(all(not a['accepted'] for a in attempts))
    def test_last_use_releases_preserve_aliases(self):
        p = self.source('node A\nout:\n    Tensor\nimpl:\n    Tensor.full(1, 1, 2.0)\nnode Sum\nin:\n    a: Tensor\n    b: Tensor\nout:\n    Tensor\nimpl:\n    Tensor.Add(a, b)\nflow Main\nout:\n    Tensor\nA() -> a\nSum(a, a) -> b\nSum(a, b)\n')
        artifact = self.dir/'app.gyb'
        self.cli('build',p,'-o',artifact)
        self.assertIn('DROP "a"',artifact.read_text())
        self.assertEqual(json.loads(self.cli('run',artifact).stdout)['data'],[6])
    def test_malformed_bytecode(self):
        p=self.dir/'bad.gyb'
        for content in ['GLYPH-BC 1\nFUNC "x"\n','GLYPH-BC 1\nENTRY "bad\\z"\n']:
            p.write_text(content)
            self.cli('run',p,ok=False)
    def test_unsupported_feature_rejected(self):
        p=self.program('1',extra='read:\n    User\n')
        self.assertIn('not implemented',self.cli('check',p,ok=False).stderr)
    def test_multiple_outputs_rejected_even_after_unit(self):
        p=self.source('node N\nout:\n    Unit\n    Int\nimpl:\n    1\nflow Main\nout:\n    Int\nN\n')
        self.assertIn('only one output',self.cli('check',p,ok=False).stderr)
    def test_graph_data_edges_and_control_order(self):
        g=json.loads(self.cli('graph',ROOT/'examples/attention.glyph').stdout)
        f=g['flows'][0]
        self.assertEqual(len([e for e in f['edges'] if e['kind']=='control']),len(f['steps'])+1)
        by_binding={s['binding']:s['id'] for s in f['steps']}
        edges={(e['from'],e['to']) for e in f['edges'] if e['kind']=='data'}
        self.assertIn((by_binding['x'],by_binding['q0']),edges)
        self.assertIn((by_binding['x'],by_binding['k0']),edges)
        self.assertIn((by_binding['x'],by_binding['v']),edges)
        self.assertNotIn((by_binding['q0'],by_binding['k0']),edges)
    def test_graph_html_is_standalone_and_escapes_source(self):
        p=self.program('1',extra='intent:\n    </script><script>alert(1)</script>\n')
        out=self.dir/'graph.html'
        self.cli('graph',p,'--html','-o',out)
        html=out.read_text()
        payload=html.split('<script id="glyph-data" type="application/json">')[1].split('</script>')[0]
        self.assertNotIn('<',payload)
        self.assertIn('</script>',json.loads(payload)['nodes']['N']['sections']['intent'][0])
        self.assertNotIn('<script src=',html)
        self.assertIn('Graph Explorer',html)
        self.cli('check',p,'--html',ok=False)
    def test_interactive_calculator(self):
        for flow,answer in [('Main','42'),('Subtract','18'),('Multiply','360'),('Divide','2')]:
            with self.subTest(flow=flow):
                p=self.cli('run',ROOT/'examples/calculator.glyph','--flow',flow,input='30\n12\n')
                self.assertIn('첫 번째 정수',p.stdout)
                self.assertEqual(p.stdout.splitlines()[-1],answer)
        p=self.cli('run',ROOT/'examples/calculator.glyph',input='abc\n',ok=False)
        self.assertIn('invalid Int',p.stderr)
        p=self.cli('run',ROOT/'examples/calculator.glyph',input='',ok=False)
        self.assertIn('IOError',p.stderr)
        p=self.cli('run',ROOT/'examples/calculator.glyph','--flow','Divide',input='1\n0\n',ok=False)
        self.assertIn('ContractViolation',p.stderr)
    def test_console_input_requires_io_effect(self):
        self.assertIn('undeclared effect IO',self.cli('check',self.program('IO.readLine()','Text'),ok=False).stderr)
    def test_source_overwrite_rejected(self):
        p=self.program('1')
        before=p.read_bytes()
        self.cli('build',p,'-o',p,ok=False)
        self.assertEqual(before,p.read_bytes())

if __name__ == '__main__':
    unittest.main(verbosity=2)
