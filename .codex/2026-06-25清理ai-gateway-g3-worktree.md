# 2026-06-25 清理 ai-gateway-g3 worktree

## 目标

用户要求确认 `.worktrees/ai-gateway-g3` 没有主仓库未保留的有效内容，并清理为只使用主仓库 `master`，避免后续继续误用 `feature/ai-gateway-g3` worktree。

## 排查命令与原因

- `git branch --show-current`
  - 确认当前操作目录是主仓库 `master`。
- `git worktree list --porcelain`
  - 确认当前只存在主仓库 worktree 和 `.worktrees/ai-gateway-g3`。
- `git status --short`
  - 查看主仓库当前保留的 G5 代码、文档、测试和构建产物状态。
- `git -C .worktrees/ai-gateway-g3 status --short`
  - 查看旧 worktree 中是否还有未提交内容。
- `git log --oneline master..feature/ai-gateway-g3`
  - 查看旧分支相对 `master` 的独有提交。
- `git log --oneline feature/ai-gateway-g3..master`
  - 查看 `master` 相对旧分支的更新提交，确认 `master` 已经推进到更新状态。
- `git diff --name-status master..feature/ai-gateway-g3`
  - 对比旧分支与 `master` 的文件差异，判断旧分支是否落后于当前主线。
- `cmp -s <worktree-file> <master-file>`
  - 对 G5 新增的 status servlet、测试和请求样例逐个做内容一致性确认。
- `git check-ignore -v .worktrees`
  - 确认 `.worktrees/` 已经被主仓库 `.gitignore` 忽略，不会再被误加入版本管理。
- `git worktree remove --force /home/muyang/workspace/sylar/.worktrees/ai-gateway-g3`
  - 在确认 G5 文件已同步、排查记录已补回后，移除旧 worktree。
- `git branch -D feature/ai-gateway-g3`
  - 删除旧 worktree 对应分支，避免后续继续切回旧分支。
- `rmdir /home/muyang/workspace/sylar/.worktrees`
  - 删除已经为空的 worktree 根目录，避免留下误用入口。

## 结论

- 主仓库当前分支是 `master`。
- `.worktrees/ai-gateway-g3` 挂在 `feature/ai-gateway-g3`，属于旧分支。
- G5 新增的 `ai_gateway_status_servlet`、`test_ai_gateway_status`、`examples/ai_gateway_request.json` 已经与主仓库内容一致。
- worktree 中缺失于主仓库的 `.codex` 排查记录已补回主仓库。
- `.worktrees/ai-gateway-g3`、空的 `.worktrees/` 目录、`feature/ai-gateway-g3` 分支均已删除。
- 复查 `git worktree list --porcelain` 和 `git branch --list` 后，只剩主仓库 `master`。
