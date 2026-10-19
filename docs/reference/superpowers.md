# 项目级 Superpowers

[Superpowers](https://github.com/obra/superpowers) **7.0.0** 提供 15 个 skills。
Claude Code 使用工程内的本地 skills，**不使用插件、市场或插件 hook**；pi 保留原有项目包安装。

## Claude Code：本地文件安装

安装布局为 `.claude/skills/<name>/SKILL.md`，各 skill 的脚本、模板和参考文件一并保留，
不是指向 pi 包缓存的软链接。`SUPERPOWERS-LICENSE` 保留上游 MIT 许可。
这些本地工具文件按 `.gitignore` 的原有规则忽略，不提交到工程仓库。

在工程根目录首次安装（需要 Git；已有同名技能时先备份并检查，避免覆盖）：

```sh
source_dir=$(mktemp -d)
git clone --depth 1 --branch v7.0.0 https://github.com/obra/superpowers.git "$source_dir/superpowers"
mkdir -p .claude/skills
cp -a "$source_dir/superpowers/skills/." .claude/skills/
cp "$source_dir/superpowers/LICENSE" .claude/skills/SUPERPOWERS-LICENSE
```

安装完成后，`.claude/skills/` 是独立副本，不再依赖下载目录或 pi。
不需要 `.claude/settings.json` 中的 `enabledPlugins` 或 `extraKnownMarketplaces`。

### 使用与检查

重启 Claude Code，运行 `/skills` 查看本地技能；也可以直接调用：

```text
/using-superpowers
/brainstorming
/systematic-debugging
```

没有插件命名空间，因此不是 `/superpowers:brainstorming`。
`CLAUDE.md` 已约定会话开始时加载本地 `using-superpowers`，并将上游文档里的
`superpowers:<name>` 映射为本地 `<name>`；不再通过 `SessionStart` 插件 hook 注入。

仅校验 skill 文件、不安装插件，可执行：

```sh
claude plugin validate .claude/skills
```

升级时先检查并备份本地修改，再从审查过的新 release 复制对应 skill 目录及许可。
如新版删除或重命名了技能，应单独核对旧目录；不要覆盖或删除其他自定义技能。

## pi：项目包（保持不变）

`.pi/settings.json` 声明 `git:github.com/obra/superpowers@v7.0.0`，固定 release tag。
包下载至 `.pi/git/github.com/obra/superpowers/`，缓存不提交。

```sh
pi install --local git:github.com/obra/superpowers@v7.0.0
pi list --approve
```

重启 pi 或执行 `/reload`，并在提示时确认项目信任。信任后加载 skills 和启动引导
extension；不信任时不加载本项目包。使用 `/skill:brainstorming` 或
`/skill:systematic-debugging`。升级时审查新版本，再用新 tag 执行上述项目安装命令。
两个客户端现在独立维护本地内容，更新时应分别检查。

## 工程约束

`CLAUDE.md`（`AGENTS.md` 是指向它的软链接）、架构边界和 QEMU 验收要求仍然有效，不能被通用 skill
流程替代。skills 可引导 agent 执行本地脚本，使用前应审查来源。
Superpowers 本身不为 pi 安装 subagent/task 工具；并行委派仍须符合当前会话的授权规则。
