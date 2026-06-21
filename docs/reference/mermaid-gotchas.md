# Mermaid 避坑清单

本仓库要求每个 ADR / 架构文档都配 Mermaid 图（见 `CLAUDE.md`）。下面是实测踩过的坑，
**都用 `mermaid.parse()` 复现/验证过**，写图前过一眼可省大量来回。

## 如何本地验证一张图（强烈建议）

不要靠肉眼判断 Mermaid 语法——它的 LALR 解析器报错列常常**溢出到块尾**，误导定位。
装核心包，喂真实 DOM 跑 `parse`：

```sh
cd /tmp && npm install mermaid@11 jsdom
```

```js
// check.mjs — 解析某 .md 里所有 ```mermaid 块
import { readFileSync } from 'node:fs';
import { JSDOM } from 'jsdom';
const dom = new JSDOM('<!DOCTYPE html><body></body>');
globalThis.window = dom.window; globalThis.document = dom.window.document;
const mermaid = (await import('mermaid')).default;
mermaid.initialize({ startOnLoad:false, securityLevel:'loose' });
const md = readFileSync(process.argv[2], 'utf8');
const blocks = [...md.matchAll(/```mermaid\n([\s\S]*?)```/g)].map(m=>m[1]);
for (let i=0;i<blocks.length;i++){
  try { await mermaid.parse(blocks[i]); console.log(`block ${i}: OK`); }
  catch(e){ console.log(`block ${i}: ERR -> ${(e.message||e).split('\n')[0]}`); }
}
```

> **必须用 jsdom 提供真实 DOM**。无头 Node 直接 `import mermaid` 会让 flowchart 误报
> `DOMPurify.addHook is not a function`——那是缺 DOM 的假阳性，不是图的错。
>
> 定位首个出错行：从第 1 行起**逐行累积**喂入 `parse`，第一个让它失败的累积片段就是真凶
> （比读报错的行号可靠）。

## 坑 1 — participant 标识符不能撞关键字

`participant Loop` 会报错：`Loop` 撞上 `loop` 块关键字（**大小写不敏感**）。错误会溢出到
图块末尾（如 `line 20: got '1'`），完全看不出是开头的 `Loop` 引起的。

避开这些保留字做 **id**（`as` 后的显示名不受限）：
`loop` `alt` `else` `opt` `par` `and` `critical` `break` `rect` `box` `end` `note`
`activate` `deactivate` `autonumber` `participant` `actor`。

```
participant Loop as vm_run 循环      ✗  id 撞 loop 关键字
participant VR   as vm_run 循环      ✓  id 改成 VR
```

## 坑 2 — sequenceDiagram 的 `as` 别名不要加双引号

双引号包裹是 **flowchart 节点**的语法，sequenceDiagram 的 `as` 别名是「自由文本到行尾」，
加引号反而报错。别名里的**裸括号/空格/点号是允许的**（只要别名整体不等于某个关键字）。

```
participant H as "handle_exit (vmexit.c)"   ✗  双引号触发解析错误
participant H as handle_exit vmexit.c       ✓
```

## 坑 3 — `box`（泳道分组）在本仓库本地工具链下解析失败

`box <颜色> <标题> … end` 用来把 participant 按分组（如特权级 EL2/EL1）画泳道。但本仓库
本地的 `mermaid@11` 对**所有** `box` 写法（`box rgb(...)`、`box transparent 标题`、
`box 标题`、纯 `box rgb(...)`）都报 `Option is not defined`，无法本地验证。

成因未定（疑似该 npm 包此特性的 bug，真实渲染器如 GitHub/VS Code 未必如此），但既然
本仓库的验收门槛是「`mermaid.parse()` 通过」，**不要用 `box`**。要表达分层/分组，改用
**别名前缀**，效果等价且可被解析器验证：

```
participant ASM as 〔EL2〕el1_irq_handler_asm
participant G   as 〔EL1〕Guest pl011 ISR
Note over ASM,G: 跨级说明用 Note 标注
```

（`docs/reference/2026-06-21-architecture-zoom-out.md` 第 4 节即此方案的实例。）

## 小结

| 坑 | 症状 | 修法 |
| --- | --- | --- |
| id 撞关键字（`Loop`/`box`/`end`…） | 报错溢出到块尾，列号误导 | id 换非关键字短名，可读名放 `as` |
| `as` 别名加双引号 | 解析错误 | 去引号；裸括号/空格本就合法 |
| `box` 泳道分组 | 本地 `mermaid@11` 报 `Option is not defined` | 改用 `〔EL2〕` 等别名前缀 + `Note` |
