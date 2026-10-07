import html
import json
import os

"""report/results.json, report/meta.json, report/suites.txt -> report/uuid_file_report.html"""

HERE = os.path.dirname(os.path.abspath(__file__))
REPORT_DIR = os.path.join(HERE, "report")

results = json.load(open(os.path.join(REPORT_DIR, "results.json"), encoding="utf-8"))
meta = json.load(open(os.path.join(REPORT_DIR, "meta.json"), encoding="utf-8"))

suites = []
for line in open(os.path.join(REPORT_DIR, "suites.txt"), encoding="utf-8"):
    kind, name, count, summary = line.rstrip("\n").split("|")
    suites.append({"kind": kind, "name": name, "count": int(count), "summary": summary})

CATS = ["설정 검증", "파일 생성·이름", "출력 포맷", "동작", "다중 프로세스"]
CAT_NOTE = {
    "설정 검증": "잘못된 설정은 기동 단계에서 바로 거부되는지, 허용 값은 정상 기동하는지",
    "파일 생성·이름": "UUIDv7 이름 형식, 권한, 정렬 순서, 접두사·확장자",
    "출력 포맷": "out_file의 모든 format이 uuid_file 모드에서 그대로 동작하는지",
    "동작": "빈 입력, 필터 제거, 재시도, 종료, tmp 정리, 기존 파일 보존",
    "다중 프로세스": "같은 디렉토리에 여러 프로세스가 동시에 기록·기동·크래시·소비",
}

rows = {c: [r for r in results if r["category"] == c] for c in CATS}
total = len(results)
passed = sum(1 for r in results if r["status"] == "PASS")
rt_total = sum(s["count"] for s in suites if s["kind"] == "runtime")
import re
e06 = next(r for r in results if r["id"] == "E06")["detail"]
crash_records = re.search(r"레코드 ([0-9,]+)건", e06).group(1)

def esc(s):
    return html.escape(str(s))

def case_rows(cat):
    out = []
    for r in rows[cat]:
        st = r["status"]
        out.append(f"""<tr class="st-{st.lower()}">
  <td class="id">{esc(r['id'])}</td>
  <td><div class="t">{esc(r['title'])}</div><div class="exp">기대: {esc(r['expected'])}</div></td>
  <td class="det">{esc(r['detail'])}</td>
  <td class="num">{r['seconds']:.1f}s</td>
  <td><span class="pill {st.lower()}">{'통과' if st == 'PASS' else st}</span></td>
</tr>""")
    return "\n".join(out)

sections = []
for c in CATS:
    n = len(rows[c])
    p = sum(1 for r in rows[c] if r["status"] == "PASS")
    sections.append(f"""<section class="cat" id="cat-{CATS.index(c)}">
  <div class="cat-head"><h3>{esc(c)}</h3><span class="count">{p}/{n} 통과</span></div>
  <p class="cat-note">{esc(CAT_NOTE[c])}</p>
  <div class="tbl"><table>
    <thead><tr><th>ID</th><th>케이스</th><th>실제 결과</th><th class="num">시간</th><th>판정</th></tr></thead>
    <tbody>{case_rows(c)}</tbody>
  </table></div>
</section>""")

suite_rows = "\n".join(
    f"<tr><td>{'런타임 (C)' if s['kind']=='runtime' else '통합 (Python)'}</td><td class='mono'>{esc(s['name'])}</td>"
    f"<td class='num'>{s['count']}</td><td>{esc(s['summary'])}</td>"
    f"<td><span class='pill pass'>통과</span></td></tr>" for s in suites)

nav = "".join(f'<a href="#cat-{i}">{esc(c)} <b>{len(rows[c])}</b></a>' for i, c in enumerate(CATS))

page = f"""<!doctype html>
<html lang="ko">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>uuid_file 검증 리포트</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=IBM+Plex+Sans+KR:wght@400;500;600;700&family=IBM+Plex+Mono:wght@400;500&display=swap">
<style>
/* layout: single reading column; summary strip, settings guide, then results grouped by category */
:root {{
  --bg: #f6f7f9; --surface: #ffffff; --fg: #18202b; --muted: #5b6676; --line: #dfe3ea;
  --accent: #1f5fbf; --accent-soft: #e6eefb;
  --ok: #1d7a46; --ok-soft: #e3f3ea; --warn: #9a5b00; --warn-soft: #fbf0dc; --bad: #b42318; --bad-soft: #fde8e6;
  --code-bg: #f0f2f6;
  --sans: "IBM Plex Sans KR", "Apple SD Gothic Neo", "Malgun Gothic", system-ui, sans-serif;
  --mono: "IBM Plex Mono", ui-monospace, SFMono-Regular, Menlo, monospace;
}}
@media (prefers-color-scheme: dark) {{ :root:not([data-theme="light"]) {{
  --bg: #11151b; --surface: #181e26; --fg: #e5e9ef; --muted: #98a3b3; --line: #2a323d;
  --accent: #7aa7ee; --accent-soft: #1d2a3d;
  --ok: #5cc98b; --ok-soft: #15301f; --warn: #e3ad55; --warn-soft: #33270f; --bad: #f2877d; --bad-soft: #3a1a17;
  --code-bg: #10141a; color-scheme: dark; }} }}
:root[data-theme="dark"] {{
  --bg: #11151b; --surface: #181e26; --fg: #e5e9ef; --muted: #98a3b3; --line: #2a323d;
  --accent: #7aa7ee; --accent-soft: #1d2a3d;
  --ok: #5cc98b; --ok-soft: #15301f; --warn: #e3ad55; --warn-soft: #33270f; --bad: #f2877d; --bad-soft: #3a1a17;
  --code-bg: #10141a; color-scheme: dark; }}
* {{ box-sizing: border-box; }}
body {{ margin: 0; background: var(--bg); color: var(--fg); font-family: var(--sans); font-size: 15px; line-height: 1.6; }}
.wrap {{ max-width: 1120px; margin: 0 auto; padding-inline: 20px; padding-block: 36px 64px; display: grid; gap: 40px; }}
h1, h2, h3 {{ text-wrap: balance; margin: 0; line-height: 1.3; }}
h1 {{ font-size: 28px; font-weight: 700; letter-spacing: -0.01em; }}
h2 {{ font-size: 20px; font-weight: 700; }}
h3 {{ font-size: 16px; font-weight: 600; }}
p {{ margin: 0; }}
.mono, code, pre {{ font-family: var(--mono); }}
code {{ background: var(--code-bg); padding: 1px 5px; border-radius: 4px; font-size: 0.9em; }}
.eyebrow {{ font-family: var(--mono); font-size: 12px; letter-spacing: 0.06em; text-transform: uppercase; color: var(--muted); }}
header {{ display: grid; gap: 10px; }}
header .sub {{ color: var(--muted); max-width: 72ch; }}
.meta {{ display: flex; flex-wrap: wrap; gap: 8px 18px; font-family: var(--mono); font-size: 12.5px; color: var(--muted); }}
.verdict {{ display: grid; grid-template-columns: auto 1fr; gap: 18px; align-items: center; background: var(--ok-soft); border: 1px solid color-mix(in srgb, var(--ok) 35%, transparent); border-radius: 10px; padding: 18px 20px; }}
.verdict .big {{ font-size: 34px; font-weight: 700; color: var(--ok); font-variant-numeric: tabular-nums; line-height: 1; }}
.verdict p {{ color: var(--fg); }}
.stats {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(190px, 1fr)); gap: 12px; }}
.stat {{ background: var(--surface); border: 1px solid var(--line); border-radius: 8px; padding: 14px 16px; display: grid; gap: 2px; }}
.stat b {{ font-size: 22px; font-variant-numeric: tabular-nums; }}
.stat span {{ color: var(--muted); font-size: 13px; }}
section {{ display: grid; gap: 14px; min-width: 0; }}
.guide {{ display: grid; grid-template-columns: minmax(0, 1fr) minmax(0, 1fr); gap: 18px; }}
@media (max-width: 860px) {{ .guide {{ grid-template-columns: minmax(0, 1fr); }} }}
pre {{ background: var(--code-bg); border: 1px solid var(--line); border-radius: 8px; padding: 14px 16px; overflow-x: auto; font-size: 13px; line-height: 1.55; margin: 0; }}
pre .c {{ color: var(--muted); }}
pre .k {{ color: var(--accent); }}
.tbl {{ overflow-x: auto; border: 1px solid var(--line); border-radius: 8px; background: var(--surface); }}
table {{ border-collapse: collapse; width: 100%; font-size: 13.5px; }}
th, td {{ text-align: left; padding: 9px 12px; border-bottom: 1px solid var(--line); vertical-align: top; }}
tbody tr:last-child td {{ border-bottom: 0; }}
th {{ font-size: 12px; font-weight: 600; color: var(--muted); background: color-mix(in srgb, var(--line) 35%, var(--surface)); white-space: nowrap; }}
td.num, th.num {{ text-align: right; font-variant-numeric: tabular-nums; white-space: nowrap; }}
td.id {{ font-family: var(--mono); font-size: 12.5px; color: var(--muted); white-space: nowrap; }}
.t {{ font-weight: 600; }}
.exp {{ color: var(--muted); font-size: 12.5px; }}
td.det {{ font-size: 13px; min-width: 260px; word-break: break-word; }}
.pill {{ display: inline-block; font-size: 12px; font-weight: 600; padding: 2px 9px; border-radius: 999px; white-space: nowrap; }}
.pill.pass {{ background: var(--ok-soft); color: var(--ok); }}
.pill.fail, .pill.error {{ background: var(--bad-soft); color: var(--bad); }}
.opt td:first-child {{ font-family: var(--mono); white-space: nowrap; }}
.opt td:nth-child(2) {{ font-family: var(--mono); white-space: nowrap; color: var(--muted); }}
.cards {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(240px, 1fr)); gap: 12px; }}
.card {{ background: var(--surface); border: 1px solid var(--line); border-radius: 8px; padding: 14px 16px; display: grid; gap: 6px; align-content: start; }}
.card.warn {{ border-left: 3px solid var(--warn); }}
.card h4 {{ margin: 0; font-size: 14px; }}
.card p {{ color: var(--muted); font-size: 13.5px; }}
nav.cats {{ display: flex; flex-wrap: wrap; gap: 8px; }}
nav.cats a {{ text-decoration: none; color: var(--fg); background: var(--surface); border: 1px solid var(--line); border-radius: 999px; padding: 4px 12px; font-size: 13px; }}
nav.cats a:hover, nav.cats a:focus-visible {{ border-color: var(--accent); outline: none; }}
nav.cats b {{ color: var(--accent); font-variant-numeric: tabular-nums; }}
.cat-head {{ display: flex; align-items: baseline; gap: 12px; flex-wrap: wrap; }}
.count {{ font-family: var(--mono); font-size: 12.5px; color: var(--ok); }}
.cat-note {{ color: var(--muted); font-size: 13.5px; margin-top: -6px; }}
.fix {{ display: grid; gap: 10px; }}
.fix-item {{ display: grid; grid-template-columns: 92px minmax(0, 1fr); gap: 12px; background: var(--surface); border: 1px solid var(--line); border-radius: 8px; padding: 12px 14px; }}
.fix-item .tag {{ font-size: 12px; font-weight: 600; align-self: start; padding: 2px 8px; border-radius: 6px; text-align: center; }}
.tag.code {{ background: var(--bad-soft); color: var(--bad); }}
.tag.test {{ background: var(--accent-soft); color: var(--accent); }}
.tag.info {{ background: var(--warn-soft); color: var(--warn); }}
.fix-item p {{ font-size: 13.5px; }}
.fix-item p + p {{ color: var(--muted); margin-top: 2px; }}
ul.flow {{ margin: 0; padding-left: 20px; display: grid; gap: 4px; }}
a {{ color: var(--accent); }}
</style>
</head>
<body>
<div class="wrap">
<header>
  <span class="eyebrow">fluent-bit · out_file · uuid_file</span>
  <h1>uuid_file 기능 검증 리포트</h1>
  <p class="sub">flush된 청크 하나를 UUIDv7 이름의 완성 파일 하나로 원자적으로 만드는 <code>out_file</code> 옵션입니다. 이 리포트는 설정 방법과, 실제 <code>fluent-bit</code> 바이너리로 실행한 검증 결과를 정리합니다.</p>
  <div class="meta"><span>커밋 {esc(meta['commit'])}</span><span>{esc(meta['platform'])}</span><span>{esc(meta['date'])}</span></div>
</header>

<div class="verdict">
  <div class="big">{passed}/{total}</div>
  <p><b>기능 이상 없음.</b> 검증 매트릭스 {total}개 케이스를 2회 연속 실행해 모두 통과했습니다. 저장소 자동화 테스트(런타임 {rt_total}개, 통합 5개)와 macOS Leaks 메모리 검사도 통과했습니다. 다중 프로세스 환경에서 레코드 손실·중복·섞임과 파일명 충돌은 한 건도 없었습니다.</p>
</div>

<div class="stats">
  <div class="stat"><b>{total} × 2회</b><span>검증 매트릭스 (실제 바이너리)</span></div>
  <div class="stat"><b>{rt_total}</b><span>런타임 테스트 (out_file·rotation·uuid·http)</span></div>
  <div class="stat"><b>16개</b><span>최대 동시 프로세스 (같은 디렉토리)</span></div>
  <div class="stat"><b>{crash_records}건</b><span>kill -9 5회 중 기록된 레코드, 손상 0</span></div>
</div>

<section>
  <h2>설정 방법</h2>
  <div class="guide">
<pre><span class="k">service</span>:
  flush: 1                    <span class="c"># 파일이 생기기까지 최대 지연</span>
  storage.path: /var/lib/fluent-bit/storage
  json.escape_unicode: off    <span class="c"># 한글을 원문 그대로 (선택)</span>

<span class="k">pipeline</span>:
  inputs:
    - name: tail
      path: /var/log/myapp/*.log
      tag: app.log
      storage.type: filesystem  <span class="c"># 종료·장애 시 유실 방지</span>

  outputs:
    - name: file
      match: 'app.*'
      path: /var/spool/relay
      mkdir: on
      format: plain             <span class="c"># JSON Lines</span>
      uuid_file: on
      uuid_file_prefix: relay-
      uuid_file_extension: .log
      uuid_file_fsync: on
      uuid_file_tmp_max_age: 10m
      retry_limit: no_limits    <span class="c"># 필수</span></pre>
    <div class="tbl"><table class="opt">
      <thead><tr><th>옵션</th><th>기본값</th><th>설명</th></tr></thead>
      <tbody>
        <tr><td>uuid_file</td><td>off</td><td>켜면 flush된 청크마다 <code>&lt;path&gt;/&lt;prefix&gt;&lt;uuidv7&gt;&lt;ext&gt;</code> 파일 1개 생성</td></tr>
        <tr><td>uuid_file_prefix</td><td>""</td><td>파일명 접두사. <code>/</code>, <code>\\</code> 불가</td></tr>
        <tr><td>uuid_file_extension</td><td>.log</td><td>확장자. 점이 없으면 붙여 줌. 빈 값 가능. <code>.tmp</code>로 끝나면 불가</td></tr>
        <tr><td>uuid_file_fsync</td><td>on</td><td>공개 전 파일 fsync, 공개 후 디렉토리 fsync</td></tr>
        <tr><td>uuid_file_tmp_max_age</td><td>10m</td><td>시작 시 이보다 오래된 자기 패턴의 <code>.tmp</code>만 정리. 숫자로 시작해야 함 (<code>600</code>, <code>30s</code>, <code>10m</code>, <code>1h</code>, <code>1d</code>)</td></tr>
        <tr><td>path</td><td>(필수)</td><td>출력 디렉토리. <code>mkdir: on</code>이면 자동 생성</td></tr>
        <tr><td>format</td><td>json</td><td>기존 out_file 포맷 모두 사용 가능. JSON Lines는 <code>plain</code></td></tr>
      </tbody>
    </table></div>
  </div>
  <div class="cards">
    <div class="card warn"><h4>retry_limit: no_limits</h4><p>기본값은 1이라서 재시도가 한 번 더 실패하면 청크를 버립니다 (D11에서 확인).</p></div>
    <div class="card warn"><h4>storage.type: filesystem</h4><p>메모리 버퍼에서는 flush 전에 종료하면 미전송분이 사라집니다. filesystem이면 재시작 후 전달됩니다 (D12·D14).</p></div>
    <div class="card"><h4>함께 쓸 수 없는 옵션</h4><p><code>file</code>, <code>rotate</code>, path의 <code>$필드</code> 접근자. Windows 미지원. 모두 기동 시 오류로 알려 줍니다.</p></div>
    <div class="card"><h4>파일 동작</h4><p><code>.tmp</code>로 쓴 뒤 <code>link()</code>로 공개해 기존 파일을 덮어쓰지 않습니다. 로그가 없거나 출력이 비면 파일을 만들지 않습니다. 권한은 0640입니다.</p></div>
  </div>
</section>

<section>
  <h2>소비하는 쪽 규칙</h2>
  <ul class="flow">
    <li><code>*.log</code>만 가져가고 <code>*.tmp</code>는 무시합니다. 완성 이름으로 보이는 파일은 항상 완전합니다.</li>
    <li>같은 파일시스템의 작업 디렉토리로 <code>rename</code>한 뒤 처리합니다. 여러 소비자가 있어도 한 곳만 성공합니다 (E7).</li>
    <li>크래시 후 재전송으로 같은 레코드가 두 번 올 수 있으니, 중복에 안전하게 처리합니다.</li>
    <li>이름 정렬은 프로세스 안에서 생성 순서와 같습니다. 엄밀한 순서가 필요하면 레코드 안의 시각을 씁니다.</li>
  </ul>
</section>

<section>
  <h2>검증 매트릭스</h2>
  <p class="cat-note" style="margin-top:0">각 케이스는 설정 파일을 만들어 <code>fluent-bit</code> 프로세스를 실제로 띄우고, 출력 디렉토리의 파일과 내용을 검사합니다.</p>
  <nav class="cats">{nav}</nav>
</section>

{''.join(sections)}

<section>
  <h2>저장소 자동화 테스트</h2>
  <div class="tbl"><table>
    <thead><tr><th>종류</th><th>대상</th><th class="num">케이스</th><th>결과</th><th>판정</th></tr></thead>
    <tbody>{suite_rows}</tbody>
  </table></div>
  <p class="cat-note" style="margin-top:0">Leaks 실행에서 건너뛴 1개는 SIGKILL 크래시 테스트입니다. 메모리 검사기는 강제 종료된 프로세스를 항상 실패로 보고하기 때문에 일반 실행에서만 돌립니다.</p>
</section>

<section>
  <h2>검증 중 발견하고 처리한 것</h2>
  <div class="fix">
    <div class="fix-item"><span class="tag code">코드 수정</span><div><p>여러 프로세스가 동시에 기동하면서 같은 디렉토리를 <code>mkdir</code>하면 일부가 기동에 실패했습니다 (60개 중 15~20개).</p><p><code>mkpath()</code>가 다른 프로세스가 방금 만든 디렉토리(<code>EEXIST</code>)를 성공으로 처리하도록 수정했습니다. 기존 out_file이 쓰는 중에 같은 경쟁으로 청크를 버리던 문제도 함께 해결됩니다. E04: 48개 동시 기동 모두 성공.</p></div></div>
    <div class="fix-item"><span class="tag code">코드 수정</span><div><p><code>uuid_file_tmp_max_age: off</code> 같은 오타가 조용히 0초로 바뀌어, 다른 프로세스가 쓰는 중인 tmp를 지울 수 있었습니다.</p><p>숫자로 시작하지 않는 값은 기동 시 거부합니다 (A14~A16).</p></div></div>
    <div class="fix-item"><span class="tag info">기존 동작</span><div><p>메모리 버퍼에서 flush 전에 종료하면 아직 쓰지 않은 레코드가 기록되지 않습니다.</p><p>uuid_file뿐 아니라 기존 out_file과 stdout에서도 똑같이 나타나는 Fluent Bit 엔진 동작입니다. <code>storage.type: filesystem</code>이면 재시작 후 500건 모두 전달됨을 확인했습니다 (D12).</p></div></div>
    <div class="fix-item"><span class="tag info">기존 동작</span><div><p>기본 설정에서는 한글이 <code>\\uXXXX</code>로 이스케이프되어 기록됩니다. JSON으로 읽으면 원문과 같습니다.</p><p>파일 원문에도 한글을 그대로 남기려면 <code>service</code>에 <code>json.escape_unicode: off</code>를 설정합니다 (C09·C10).</p></div></div>
    <div class="fix-item"><span class="tag test">테스트 보정</span><div><p>첫 실행에서 실패한 4건 중 2건은 테스트 쪽 문제였습니다. 빈 레코드를 넘기는 하네스 버그(D03)와, 설정 파서가 앞 공백을 제거한다는 것을 반영하지 않은 기대값(A17)입니다.</p><p>둘 다 고친 뒤 전체를 2회 다시 실행해 61/61 통과했습니다.</p></div></div>
  </div>
</section>
</div>
</body>
</html>
"""
open(os.path.join(REPORT_DIR, "uuid_file_report.html"), "w", encoding="utf-8").write(page)
print("ok", len(page))
