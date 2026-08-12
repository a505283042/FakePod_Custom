---
alwaysApply: true
scene: git_message
---

# Git 提交信息规则

## 语言要求
- **提交信息主体（subject）必须使用中文书写**
- type、scope 保持英文小写
- 正文（body）与脚注（footer）同样使用中文

## 提交信息格式

采用 Conventional Commits 规范，结构如下：

```
<type>(<scope>): <subject>

<body>

<footer>
```

### type 取值（英文小写，必须使用以下之一）
- `feat`     新功能
- `fix`      缺陷修复
- `refactor` 重构（不改变外部行为）
- `perf`     性能优化
- `docs`     文档变更
- `chore`    构建/依赖/版本号等杂项
- `style`    格式调整（不改逻辑）
- `test`     测试相关
- `build`    构建系统或外部依赖
- `ci`       CI 配置
- `revert`   回滚提交

### scope（可选）
- 使用英文小写，表示受影响的模块或子系统
- 示例：`display`、`boot`、`player`、`spectrum`、`gesture`、`audio`、`hw`

### subject（必填）
- 简明描述本次变更，**中文书写**
- 不超过 50 个汉字为宜
- 结尾不加句号
- 多个变更点用中文逗号「，」分隔
- 涉及版本号、编号（如 R.37、P1.5.3.2）原样保留

### body（可选）
- 详细说明「为什么改」与「怎么改」，使用中文
- 每行不超过 72 字符
- 多条要点用「- 」开头列表

### footer（可选）
- BREAKING CHANGE：以 `BREAKING CHANGE:` 开头，后接中文说明
- 关联议题：`Closes #123`、`Refs #456`

## 示例

```
feat(display): 实现BoundedSPI快路径并升级固件版本
```

```
refactor(boot): 重构启动流程，拆分启动核心与业务初始化

- 将启动核心与业务初始化解耦，便于后续移植
- 移除冗余的延时等待，改用事件驱动
```

```
chore: 正式版本升级至P1.5.3.2R.38.5.3.1，修复DAC复位时序
```

```
fix(audio): 修复DAC复位时序异常导致的首帧爆音

BREAKING CHANGE: DAC 初始化时序调整后，外部功放使能时序需相应延后
```

## 禁止事项
- 不要使用英文书写 subject 主体
- 不要在 subject 末尾加句号或感叹号
- 不要使用模糊描述（如「update code」「fix bug」）
- 不要在单条提交中混合多个不相关的 type
- 不要在 subject 中包含提交哈希或作者信息
