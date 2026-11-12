# web_render 已知问题与修复记录

更新日期：2026-11-08

## 一、本轮修复的三项回归

### 1. 圆角图形显示为直角

根因：`border-radius` 简写在样式计算阶段总是展开成四条长写属性，只写入四角字段；
绘制层的原子盒与图片路径读取标量 `border_radius`，该字段因此恒为 0，
背景按直角填充，而边框仍按四角半径描边，形成“圆角边框 + 直角底色”。

修复内容：
- `ComputedStyle` 的四角半径改为 `CornerRadius` 结构，分别保存水平、垂直半径与
  百分比标记、calc 像素偏移，百分比在绘制阶段按边框盒宽、高解析。
- `border-radius` 简写支持斜杠语法（`10px / 20px`）与百分比；长写属性支持 1 至 2 个长度。
- 绘制层统一由 `StyleCornerRadii(style, rect)` 解析四角半径，原子盒、行内盒、
  块级盒的背景、内容裁剪与边框共用同一套圆角几何。
- 四角半径按 CSS 规则等比收缩，避免相邻圆角重叠。
- 圆角填充与圆角描边改为整数像素行扫描线，并合并内缩相同的相邻行；
  修掉了逐行填充在像素边界重叠导致颜色变浅、以及原子盒上下圆角区出现接缝的问题。
- 四角半径不同的边框改为逐行绘制圆角环；四角一致的圆角仍走着色器描边。
- 图片内容裁剪改用内容区内轮廓半径（外半径减边框宽度）。
- `RenderDevice` 新增 `DrawImageSubRect`，`object-fit: cover` 只绘制与内容区
  相交的纹理范围，圆角裁剪不再出现直角残影；背景图的边缘图块不再被压缩。
- 顺带修正匿名盒样式：不再残留元素自身的四角半径。

### 2. 按钮事件不生效

根因与修复：
1. 命中按钮内部的元素时只对命中元素本身执行默认行为。新增
   `FindActivatableAncestor`，预览窗口与命令行点击路径都先向上查找最近的可激活控件，
   再执行 submit/reset/change 等默认动作。
2. 脚本宿主原先只识别 `addEventListener`。现在支持内联事件属性（`onclick="..."`）
   与事件处理器属性赋值（`element.onclick = fn`），两者与监听器共用同一条派发路径：
   属性文本按函数体编译并缓存，目标节点上的处理器先于监听器执行，`this` 指向当前节点。
3. 命令行模拟点击原先只派发脚本 `click`，不执行控件默认动作。现在与预览窗口一致：
   先执行默认动作并派发 change/submit/reset，再派发 click 并重新布局；
   表单初始值在载入后统一快照，供 reset 恢复。

### 3. 窗口拖动缩放卡顿

根因：每次 `WM_SIZE` 都完整重排文档，并在绘制时销毁重建渲染设备与图片缓存，
拖动边框时每秒触发数十次。

修复内容：
- 客户区尺寸变化按帧节流：`WM_ENTERSIZEMOVE` 到 `WM_EXITSIZEMOVE` 期间每约 16ms
  按最新尺寸重排一次；节流定时器已在等待时不重置，避免连续的 `WM_SIZE` 把重排
  一直推迟到鼠标停下。结束拖动时立即应用最后一次尺寸。
- 重排间隙把上一帧按原像素尺寸贴到客户区左上角，未覆盖区域留白；不再缩放位图，
  拖动过程中不会出现明显的画面拉伸变形。
- 渲染设备复用 `RenderDevice::Resize`，只重建渲染目标，已上传的纹理与图片缓存保持有效。
- 缩放时保留当前滚动位置与页码（越界才收敛到有效范围），不再回到第 0 页。
- 处理 `WM_ERASEBKGND` 跳过背景擦除，减少闪烁。
- 设置 `WEB_RENDER_PROFILE` 时输出 `PROF resize layout=<毫秒>` 与 `PROF paint` 行，
  便于测量拖动缩放的重排与绘制耗时。

## 二、尚未实现或仅部分实现

- `transform` 的 rotate/skew/matrix；`clip-path` 的 circle/ellipse/polygon。
- `overflow: auto/scroll` 的滚动容器与滚动条；`position: sticky` 的真实滚动粘附。
- 多背景层叠加、`background-blend-mode`、`backdrop-filter`、模糊类滤镜。
- RTL/双向文本、`writing-mode`、多列布局。
- SVG 的 `use`、`symbol`、`marker`、`clipPath`、`mask`、`filter`、`foreignObject`，
  以及文本描边（`paint-order: stroke` 的描边部分）与 `textPath`。SVG 命中测试已支持
  图元、文本包围盒与 `pointer-events`，但不判断字形轮廓，也不支持 `pointer-events`
  在 HTML 元素上的滚动/裁剪等完整语义。
- CSS 动画与过渡的播放（目前只解析规则，静态渲染取初始值）。
- `@media` 查询与响应式断点：样式表按单一样式表解析，不随视口切换规则。
- `grid-auto-flow: dense`、`subgrid`、命名网格线。
- 图片与画布的着色器只接受单一半径：四角半径不同或为椭圆角时，内容裁剪取最大半径。
- `overflow: auto/scroll` 的滚动容器、`range`/`color` 的独立控件类型与行内基线度量
  （见下方历史记录第三条）。
- `getComputedStyle`：脚本宿主未提供样式查询接口。
- `dataset` 为只读视图：由 `data-*` 属性生成对象，写入不会回写属性。

## 二之二、真实页面测试补充修复（2026-11-07）

用 `E:\code\web\电路图设计`（SVG 编辑器）验证时补齐的缺口：

1. **中文路径崩溃**：命令行参数由 CRT 按本地代码页转换，直接当 UTF-8 使用会使
   `std::filesystem::u8path` 抛异常。改为 `CommandLineToArgvW` 取宽字符命令行再转
   UTF-8；路径拼接与基准目录一律使用 `path::u8string()`，不再混用本地代码页的 `string()`。
2. **百分比高度**：根元素与含确定高度祖先的百分比高度改按包含块高度解析
   （此前按宽度解析）。`LayoutBlock` 新增包含块高度与高度覆盖参数。
3. **弹性项目最终尺寸**：列方向按 grow/shrink、行方向按 stretch 确定项目高度后，
   用最终内容高度重新排版该项目，子元素的百分比高度、定位包含块与背景填充才正确。
4. **外链脚本**：`CollectScripts` 按文档顺序加载 `<script src>`（相对文档目录）。
5. **脚本宿主补充**：`classList`（add/remove/toggle/contains）、`dataset` 只读视图、
   `window.addEventListener`（事件冒泡结束后触发）、元素级
   `querySelector`/`querySelectorAll`（限定子树，结果为数组）、异常信息附带 `stack`。
6. 验证：三个样例内容高与像素差异保持基线不变，覆盖测试 7 页差异 0%，
   `build\tmp\web_render\flex_column.html` 的列式 flex 布局（顶栏/三栏/底栏）铺满视口高度。

## 二之三、内联 SVG 渲染（2026-11-07）

`svg` 按替换元素参与布局（块级与行内原子盒两条路径），宽高优先取 CSS，
其次取 `width`/`height` 属性，缺省 300×150；子树不再进入 HTML 排版。
布局阶段为 SVG 根及其全部后代计算样式并存入 `LayoutResult::svg_styles`，
绘制与字形收集按该表读取 CSS 类、内联样式与 `:root` 自定义属性。

新增 `src/svg_render.cpp`，在页面坐标系中用扫描线栅格化（垂直 4 次采样、
水平分数覆盖）后输出矩形跨度，因此不需要扩展两种后端的图元接口。支持：

- 变换：`translate`/`scale`/`rotate`/`matrix`/`skewX`/`skewY`，`viewBox` 的
  `xMidYMid meet` 与 `none`，`preserveAspectRatio` 缺省值。
- 图元：`rect`（含 `rx`/`ry` 圆角）、`circle`、`ellipse`、`line`、`polyline`、
  `polygon`、`path`（M/L/H/V/C/S/Q/T/A/Z 及相对命令，曲线与圆弧折线化）。
- 填充：纯色、`fill-opacity`、`fill-rule` 采用非零环绕、线性与径向渐变
  （`objectBoundingBox`/`userSpaceOnUse`、多色标与 `stop-opacity`）、
  `pattern` 平铺（`userSpaceOnUse`/`objectBoundingBox`）。
- 描边：`stroke-width`/`stroke-opacity`、线端 butt/round/square、连接
  miter/round/bevel、`stroke-dasharray`/`stroke-dashoffset`（静态相位）。
- 文本：`font-family`/`font-size`/`font-weight`/`font-style`、`text-anchor`、
  `dominant-baseline`（alphabetic/central/middle/hanging）、`dx`/`dy`；
  字形在 `UpdateFontAtlas` 之前由 `CollectSvgBoxGlyphs` 收集。
- 继承与优先级：自身 CSS 声明 → 自身呈现属性 → 父级继承值；`opacity`
  不继承，按元素累乘。
- `getBoundingClientRect`：脚本宿主新增几何回调，元素边框盒由引擎按当前
  布局结果查询；无窗口 `--js` 模式在脚本执行后推进一帧，使页面在
  `requestAnimationFrame` 中完成的初始缩放与居中生效。
- 命中测试与交互：`HitTestPage` 在 SVG 布局盒内继续测试图元（矩形、圆、椭圆、
  折线、路径、文本包围盒），读取 CSS 与呈现属性中的 `pointer-events`；
  脚本宿主补充 `closest`、`pointerdown`/`pointermove`/`pointerup`/`wheel`/
  `contextmenu`、`preventDefault`/`stopPropagation` 与 `button`/`metaKey`/`deltaY`；
  预览窗口派发对应的指针事件，滚轮在页面调用 `preventDefault` 时不翻页。
- `localStorage`：提供 `getItem`/`setItem`/`removeItem`/`clear`/`key`/`length`，
  按文档绝对路径保存在 `%LOCALAPPDATA%\web_render\local_storage` 下的文本文件中，
  十六进制编码键值；页面保存的设计状态因此可以跨运行保留。
- 交互性能：SVG 子树内部的变更（图元、变换、class）通过独立脏标记走增量刷新，
  不重新排版 HTML；SVG 计算样式按 class/style/id 签名复用，完整布局同样复用
  上一版结果。图案填充的内容在铺排前只解析一次，内容完全位于图块内时跳过
  逐块裁剪。需要重新匹配全部 SVG 样式的场景仅为样式签名变化的元素及其后代。
  该复用假设样式表内容在运行期间不变：脚本改写 `<style>` 文本不会使已复用的
  SVG 样式失效。设置环境变量 `WEB_RENDER_PROFILE` 可输出交互各阶段耗时。

## 二之四、预览窗口垂直滚动（2026-11-07）

- 内容高于客户区时右侧自动显示滚动条；滑块高度按视口与内容高度之比计算，
  最短 28 像素。拖动滑块或点击轨道可跳转，滚动条交互期间不派发页面点击事件。
- 滚轮每格滚动 72 像素，↑/↓ 48 像素，PageUp/PageDown 滚动 90% 视口，
  Home/End 直达首尾；←/→ 仍按页跳转。页面调用 `preventDefault` 时滚轮不平移。
- 布局仍按页切分；视口跨越页边界时分别渲染相邻两页，再按滚动偏移逐行拼接，
  不再以整页为单位跳变。命中测试、文本选区、下拉浮层与内嵌文档坐标都计入偏移。
- 滚动条为覆盖式（右侧 8 像素），不改变布局宽度；内容短于视口时自动隐藏。

## 二之五、Canvas 2D 与脚本宿主补充（2026-11-08）

新增 Canvas API：

- `getImageData` / `putImageData` / `createImageData`：ImageData 的 `data` 为
  `Uint8ClampedArray`；坐标为设备像素，忽略当前变换、裁剪与全局透明度；越界读取
  返回透明黑；单次读写限制 64 MiB。`putImageData` 支持 3 参数与带脏矩形的 7 参数形式。
- `clip()`：以当前路径按 nonzero 规则生成逐像素覆盖率掩码，多次调用取交集；
  空路径产生空裁剪区域；裁剪区域参与 `save` / `restore`（状态间共享掩码，
  修改前复制）。画布尺寸变化时丢弃裁剪区域。
- `globalAlpha`：只接受 0..1 之间的有限值，参与填充、描边、文本、图片与渐变的合成。
- `lineCap`：butt / round / square；开放子路径的首尾端点按该值处理。
- `lineJoin`：miter / round / bevel；miter 在斜接长度超过 `miterLimit` 倍半线宽时
  退化为 bevel。描边几何改为多边形轮廓（线段四边形、连接、端帽），不再使用距离场。
- `miterLimit`：只接受不小于 1 的有限值。
- `setLineDash` / `getLineDash` / `lineDashOffset`：虚线按用户坐标长度给出，描边时
  按当前缩放换算到设备像素；闭合路径首尾相接的实线段会合并。
- 路径补充：`rect`、`ellipse`（含旋转与方向）、`arcTo`；`isPointInPath` 支持
  nonzero 与 evenodd 规则，并计入当前变换。
- `createPattern`：源为 canvas 或 img，重复模式支持 repeat / repeat-x / repeat-y /
  no-repeat；创建时记录当前变换，绘制时按其逆矩阵映射到图案像素。
- `toDataURL`：使用 WIC 在内存中编码 PNG 再转 Base64；非 `image/png` 类型返回
  `data:,`。
- `data:` 内联图像：支持 Base64 与百分号编码，画布 `drawImage` 与 HTML/CSS 图片
  共用内存解码路径。
- `globalCompositeOperation`：source-over / source-in / source-out / source-atop /
  destination-over / destination-in / destination-out / destination-atop / lighter /
  copy / xor。清除类操作（source-in、source-out、destination-in、copy）会处理源
  覆盖范围以外的像素；文本先累积整段覆盖率再统一合成一次。
- 阴影：`shadowColor` / `shadowBlur` / `shadowOffsetX` / `shadowOffsetY`，
  作用于填充、描边、文本与 drawImage。阴影由源覆盖率或图像 Alpha 生成，
  按整数偏移搬移后用三次盒式模糊逼近高斯模糊（标准差为 shadowBlur 的一半），
  再以阴影颜色合成；全部属性进入 save / restore 与画布重置流程。
  阴影与源分别参与当前合成操作，对清除类操作（如 copy）的结果与浏览器把阴影
  并入源再合成不完全一致。
- `textAlign`：start / end / left / right / center；`direction` 为 ltr 或 rtl。
  rtl 时字符从右向左排布，start/end 随方向翻转，left/right 保持物理方向；
  不执行 Unicode 双向算法分段与字形整形，混排文本按单一方向处理。
- `textBaseline`：alphabetic / top / hanging / middle / ideographic / bottom，
  偏移量由字体度量计算（top 与 hanging 取 ascent，bottom 与 ideographic 取 descent 下沿）。
- `canvas.width` / `canvas.height`：读写位图尺寸并同步属性；调整尺寸会清空位图、
  裁剪区域与全部绘制状态（变换、样式、字体、当前路径、状态栈）；0 视为合法尺寸
  （不绘制）；负数、非有限值与超过 16384 的值被忽略；非 canvas 元素读写返回 undefined。
- 上述新增状态全部进入 `save` / `restore`。

脚本宿主：

- `getAttribute` 在属性缺失时返回 `null`，属性存在但值为空时返回空字符串。
- 每次脚本执行、事件派发、`setTimeout`、`setInterval` 与 `requestAnimationFrame`
  回调结束后驱动 QuickJS 作业队列，`Promise` 的 then/catch/finally 与 `await`
  之后的代码都会执行。
- 元素级 `getElementsByTagName`：只查询后代元素，不含接收者自身；document 与元素
  共用同一实现。
- `setInterval` / `clearInterval`：重复定时器按固定间隔排队，零间隔按 1 毫秒处理。

Canvas 尚未实现：

- Unicode 双向算法分段与阿拉伯文等字形的整形：`direction: rtl` 已支持基本
  从右向左排布，但混排文本不按双向算法重新排序，也不做连写变形。

## 二之六、CSS 颜色与下拉框补充（2026-11-08）

- `hsl()` / `hsla()`：色相支持无单位数值与 `deg`、`grad`、`rad`、`turn`；
  饱和度与亮度按百分比解析；Alpha 支持数值与百分比。
- 命名颜色扩展到 CSS Color 4 的完整集合（含 `transparent`、`gray`/`grey` 等
  同义拼写，共 148 项）；颜色表改为 0xAARRGGBB 整数存储，避免浮点字面量误差。
- `select` 的 option 收集改为递归进入 `optgroup`：下拉框显示文本与脚本侧
  `select.value` / 当前选中项使用同一文档顺序，HTML5 套件的 optgroup 用例通过。

## 二之七、尺寸约束、阴影、占位文本与分页（2026-11-08）

- `box-sizing`：`content-box` 与 `border-box` 均按边框盒换算内容尺寸；块级、
  弹性项目与栅格单元共用同一套换算。
- `min-width` / `max-width` / `min-height` / `max-height`：宽度相对包含块宽度
  解析，高度在包含块高度确定时相对高度、否则沿用宽度基准；百分比与
  `border-box` 同时生效。
- 块级子项的 `margin-left` / `margin-right: auto` 吸收剩余空间：两侧均为 auto
  时居中，仅一侧为 auto 时靠另一侧对齐。
- 弹性盒行方向主轴与列方向交叉轴的尺寸都按 `border-box` 扣除内边距与边框；
  列方向交叉轴的拉伸宽度同样换算为内容宽度，栅格单元宽度交由块级布局自行解析。
- `position: sticky` 在没有滚动容器信息时按普通流排版，不再应用 `top` / `left`
  偏移，避免侧边栏整体下移。
- 行内原子盒（行内块按钮等）补绘渐变与背景图片；此前只填充背景色，导致普通
  行内流中的渐变按钮没有背景。
- 盒阴影的外阴影改为分层递减不透明度：单层不透明度按叠加关系反解，层数取
  模糊半径的两倍，消除最外层出现等高实心色带的问题。
- `textarea` / `input` 的值设为空且存在 `placeholder` 属性时绘制灰色占位文本；
  占位文本的字形在图集收集阶段单独加入，textarea 因不走表单控件排版路径而
  按标签名识别。`textarea` 的 `rows` 属性决定默认可见行数（缺省 2 行），
  内容不足时仍保留对应高度；`cols` 属性尚未实现。
- 分页候选：带背景、边框、圆角或阴影且能整体放入一页的叶子块（按钮、列表项
  等）不可拆分，只把块盒顶部作为切页点，避免背景留在上一页而文字落到下一页。
  绝对定位子盒不影响该判断。
- 浮动布局：`float: left/right` 的元素按浮动行靠边依次排列，超出容器宽度时
  换行；浮动不推进普通流游标，也不参与外边距合并。`clear: left/right/both`
  与普通流块级盒统一避让尚未结束的浮动行。行内文字暂不支持绕排，普通流中的
  文字块会整体排在浮动下方。

## 二之八、输入法、交互状态与 CSS 动画（2026-11-08）

- 输入法：处理 `WM_IME_STARTCOMPOSITION`、`WM_IME_COMPOSITION`、`WM_IME_ENDCOMPOSITION`
  与 `WM_IME_NOTIFY`。组合串作为预编辑文本写入焦点控件（不派发 `input`），
  提交结果时按普通编辑替换选区并派发一次 `input`；放弃组合时恢复原文。
- 候选窗口定位：按光标在文档中的位置换算为客户区与屏幕坐标，调用
  `ImmSetCompositionWindow` 与 `ImmSetCandidateWindow` 设置组合窗口与候选窗口
  位置；滚动与组合串更新时同步。`WM_IME_SETCONTEXT` 隐藏系统组合窗口，
  组合串由控件内部绘制。
- 交互状态：元素保存 `hovered` / `active` / `focused` 标记，选择器匹配
  `:hover`、`:active`、`:focus`、`:focus-visible`、`:focus-within`。指针移动
  设置悬停元素及其祖先，按下/松开设置或清除按压链，焦点切换同步 `:focus`；
  状态变化时重新匹配样式并排版。`WM_MOUSELEAVE` 清除悬停。
- CSS 动画：文本级提取 `@keyframes`（名称统一小写，支持 `from`、`to`、
  百分比与逗号分隔的多选择器）；解析 `animation` 简写的时长、延迟、缓动、
  `infinite`、方向与填充方式；缓动使用三次贝塞尔求解。
- 动画采样：按动画时钟取相邻关键帧，分别生成计算样式后插值背景色、文字色、
  四边边框色、`opacity`、`transform` 的平移与缩放、`box-shadow` 与
  `text-shadow`；`alternate` / `alternate-reverse` 按迭代次数翻转，
  `forwards` / `backwards` / `both` 处理延迟与结束时刻。
- 动画驱动：窗口模式由 16ms 帧定时器推进时钟，存在活动动画时重新采样样式、
  排版并重绘；静态截图固定取 2 秒时刻，使一次性入场动画完成。
- 已知限制：`transform` 尚未支持 `rotate`、`skew`；`background-position`
  动画不会移动渐变；`filter`、`clip-path` 等属性不参与动画；关键帧中的
  `width` / `height` 不参与插值（动画不改变布局尺寸）。

## 二之九、文档生命周期与文档接口（2026-11-08）

- `DOMContentLoaded`：页面脚本执行完成后在文档节点上派发一次，随后冒泡到
  `window`；`document.readyState` 在执行脚本阶段为 `loading`，事件处理器
  执行期间为 `interactive`，派发结束后变为 `complete`。页面把初始化逻辑
  注册在该事件上的写法（侧边栏导航、各工具按钮）由此生效。
- `document.cookie`：以“名称=值”形式保存，读取时用 `"; "` 连接；`expires`、
  `path` 等属性参与解析但不参与存储，也不做跨会话持久化。
- `document.activeElement`：返回文档中第一个带焦点标记的元素，没有焦点时
  返回 `null`；点击可激活控件（按钮、链接、下拉框等）时焦点跟随该控件，
  与浏览器一致，页面据此判断触发来源。
- `element.select()`：文本框与输入框记录当前文本；`document.execCommand("copy")`
  把记录的文本写入系统剪贴板，其它命令返回 `false`。复制按钮因此可以工作。
- 绝对定位与固定定位伪元素脱离行内流，按其包含块生成独立盒子；按钮的水波纹
  （`.btn:active::after`，宽高 0 → 300px）不再撑大宿主按钮，侧边栏的
  定位装饰线也按 `scaleY` 状态显示。

## 三、构建、验证与基线

构建（本项目为独立实验目标，需要在实验目录外单独配置构建目录）：

```powershell
cmake -S experiments\web_render -B build\web_render -G "Visual Studio 17 2022" -A x64
cmake --build build\web_render --config Release --target web_render
```

依赖：`third_party\quickjs`（quickjs-ng 源码）、re2c 与 bison（`E:\code\tools`）。

可执行文件：`build\web_render\Release\web_render.exe`

单页双后端渲染并比较：

```powershell
build\web_render\Release\web_render.exe --html experiments\web_render\samples\article.html `
  --out build\tmp\web_render\reg\article --backend both --width 1000 --height 1600 --page -1 --compare --js
```

固定基线（内容高，视口 1000）：

| 页面 | 内容高 |
| --- | --- |
| samples/article.html | 1255.95 |
| samples/features.html | 2421.73 |
| samples/town.html | 1665.5 |

双后端像素差异（差值超过 8 的像素比例）：
article 0%、features 0.0148%、town 0.329%（差异集中在表格边框、圆角边缘与
多层盒阴影的抗锯齿处理，属于两个后端光栅化差异）。

交互回归：
- 圆角：`build\tmp\web_render\rounded_test.html`（原生按钮 3px、自定义圆角、`20px 0 20px 0`、
  `60px / 30px` 椭圆角、`50%` 圆形、胶囊按钮、圆角图片）。
- 事件：`build\tmp\web_render\events_test.html`（内联 onclick、嵌套子元素、属性赋值 onclick、
  嵌套 span 的提交按钮、重置按钮）与 `build\tmp\web_render\events_nested.html`（块级子元素）。
- 缩放：`build\tmp\web_render\window_resize_test.ps1`（拖动缩放 24 次后检查进程存活、客户区尺寸与
  消息往返时延）。

## 四、历史记录（2026-11-06，仍未处理）

表单控件与周围文字的垂直位置不一致；`range`/`color` 被归入文本输入类型，
点击进入文本编辑；无文本行的行内块按底边计算基线导致行盒抬高；
`overflow` 尚无真正的滚动容器。修复时需同时核对上表三份基线的内容高。
