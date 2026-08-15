"""OpenMV 4.5.0 / RA8D1 药名识别 V6.3.4 彩色 LCD + UTF-8 UART 程序。

TF 卡根目录需要：
    drug_classifier_v6_uint8.tflite
    charset_v4_500.txt
    drug_names_v4.txt
    main.py（本文件复制后改名）

V6.3.0 使用适合 OpenMV 4.5.0 / RA8D1 的低内存两级定位流程：
固定宽度的 6:1 窗口先按 LAB 亮度对比度粗搜 -> 局部大字连通域精框 -> 药名分类复核。
OV5640 使用约 1.5 倍居中读出缩放并保持 QVGA 输出，在清晰度和长标题视野间折中。
传感器只保留一张 RGB565 原帧；LCD 显示彩色画面，绿色框在推理完成后绘制。
灰度模型由 tf.classify 从彩色原帧 ROI 直接生成 192x32 单通道输入，不复制整帧。
不再依赖连通域作为主定位结果，避免药盒边框、品牌图案和英文小字把候选行粘连。
禁止 image.copy() 同时进行 ROI 缩放，避免旧固件出现 Invalid ROI dimensions。
V6.3.4 LCD 始终显示彩色画面和绿色框；未识别状态只通过 UART(2) 发送 UTF-8 文本。
"""

import gc
import os
import time


MODEL_NAME = "drug_classifier_v6_uint8.tflite"
CHARSET_NAME = "charset_v4_500.txt"
DRUG_NAMES_NAME = "drug_names_v4.txt"
BUILD_ID = "V6.3.4-20260806-UART-ONLY-UNKNOWN"
EXPECTED_MODEL_SIZE = 294904
EXPECTED_MODEL_SHA256 = b"\x57\xa0\xa2\x28\x8c\xe8\xc6\x16\x73\x5e\x93\xf5\x1a\x29\x49\x84\x3d\x45\xc3\x65\x6e\xe1\x46\x86\x88\xfe\x5e\x6c\xfe\x49\x43\x91"
EXPECTED_CHARSET_SHA256 = b"\xeb\x29\x8d\xe1\x7e\x5a\x48\x05\x56\x33\xaf\x40\x01\x14\xd9\xc4\xc2\xc6\x21\xeb\x79\xe0\xbc\xc7\x00\xd8\xe6\x1b\xf9\x34\x01\xc2"
EXPECTED_DRUG_NAMES_SHA256 = b"\x95\xa3\x3c\xb4\x41\xfe\x5f\xc3\xd6\x03\x4c\x91\xe4\xf9\x4e\x0c\xd7\xee\x8f\x84\x90\x67\xbe\xa7\x61\x5e\x21\xc6\x4f\x65\xf8\x4c"
EXPECTED_CHARSET_SENTINELS = (
    (98, "枸"),
    (99, "橼"),
    (45, "酸"),
    (18, "莫"),
    (49, "沙"),
    (100, "必"),
    (101, "利"),
    (27, "片"),
)

IMAGE_WIDTH = 192
IMAGE_HEIGHT = 32
FRAME_WIDTH = 320
FRAME_HEIGHT = 240
TIME_STEPS = 48
CHARSET_SIZE = 500
DRUG_CLASS_COUNT = 34
UNKNOWN_CLASS_INDEX = 34
MODEL_OUTPUT_COUNT = 35
CLASS_CONFIDENCE_MIN = 0.75
CLASS_MARGIN_MIN = 0.25
# 最终拒识门槛与定位提示门槛分离：像截图中 78.5%/66.8% 的正确标题
# 可以直接接受；更低但明显指向某个目标类别的结果只帮助选框，不进入投票。
LOCATOR_HINT_CONFIDENCE_MIN = 0.58
LOCATOR_HINT_MARGIN_MIN = 0.20
LOCATOR_STRONG_CONFIDENCE_MIN = 0.72
LOCATOR_STRONG_MARGIN_MIN = 0.35

NO_CANDIDATE = 255
HISTORY_SIZE = 7
STATUS_INTERVAL = 10
MEMORY_REPORT_INTERVAL = 64
FRAME_INTEGRITY_INTERVAL = 30
FPS_REPORT_INTERVAL = 20
DRAW_TITLE_BOX = True
COLOR_CAPTURE = True
TITLE_BOX_COLOR = (0, 255, 0)
LCD_ENABLED = True
LCD_INTERVAL = 1
# 该 RA8D1 厂商固件存在兼容约定：UART 波特率参数 0 实际对应 115200。
# 协议为 UTF-8 文本行：稳定时发送中文药名，未知/尚未稳定时发送“未识别”。
UART_ENABLED = True
UART_PORT = 2
UART_BAUDRATE = 0
UART_TIMEOUT_CHAR = 200
UART_SEND_INTERVAL = 1
UART_UNKNOWN_TEXT = "未识别到药品"
UART_LINE_ENDING = "\r\n"
# RA8D1 固件的 IDE 自动预览只有约 32 KB JPEG 缓冲区。关闭自动发布，
# 在 LCD 显示结束后以低质量手动发布完整彩色 QVGA JPEG。
MANUAL_IDE_PREVIEW = True
PREVIEW_INTERVAL = 1
PREVIEW_QUALITY = 25
PREVIEW_BUFFER_LIMIT = 32704
MODEL_LOAD_TO_FB = True
# OV5640 原生阵列为 2592x1944。居中读取 1728x1296 再由传感器输出 QVGA，
# 相当于约 1.5 倍缩放：比全视野多 50% 标题像素，又不像旧 VGA 320x240
# 中央裁剪那样达到约 2 倍并容易截掉长药名。设为 None 可恢复 QVGA 全视野。
CAMERA_READOUT_SIZE = (1728, 1296)
# V4 模型的真实训练帧是传感器自然灰度、低对比且略柔和的图像。
# 额外 contrast=2 和反锐化会改变笔画灰度分布，真机上表现为整串稳定乱码。
CAMERA_CONTRAST = 0
OCR_UNSHARP = False
PRINT_RAW_IDS = False
PRINT_CANDIDATE = False
# 真机诊断：等待稳定时每10帧打印原始文字、最近药名和编辑距离。
# 问题确认后可改为 False，以释放字符表占用的少量堆内存。
PRINT_RAW_STATUS = True
# 已经手动调焦时必须关闭，否则启动阶段的 OV5640 VCM 自动对焦会覆盖焦点。
AUTO_FOCUS = False
CUSTOM_EXPOSURE_CONTROL = False
AUTO_TITLE_ROI = True
# 自动定位失败时显示的引导框。根据当前实拍，药名位于画面中上部；
# 框仍严格保持模型所需的 6:1 比例。
DEFAULT_TITLE_ROI = (40, 44, 240, 40)
TITLE_SEARCH_ROI = (4, 12, 312, 112)
TITLE_DETECT_INTERVAL = 12
TITLE_HOLD_FRAMES = 20
VISUAL_LOCK_HITS = 2
VISUAL_SWITCH_HITS = 3
VISUAL_LOST_FRAMES = 8
OCR_SEARCH_INTERVAL = 3
OCR_GLOBAL_PROBE_LIMIT = 4
OCR_TRACK_PROBE_LIMIT = 4
TEXT_PROPOSAL_INTERVAL = 9
GLOBAL_VISUAL_PROPOSAL_LIMIT = 12

# 粗窗口只负责搜索，OCR 前再在其内部找大字基线并生成紧贴文字的 6:1 精框。
# 精框只增加一次小 ROI 的 find_blobs，不增加模型推理次数。
REFINE_FULL_WIDTH_SEARCH = True
REFINE_MARGIN_X = 38
REFINE_MARGIN_Y = 10
REFINE_COMPONENT_LIMIT = 64
REFINE_MIN_COMPONENT_WIDTH = 2
REFINE_MIN_COMPONENT_HEIGHT = 6
REFINE_MAX_COMPONENT_WIDTH = 96
REFINE_MAX_COMPONENT_HEIGHT = 54
REFINE_MIN_LINE_WIDTH = 40
REFINE_MIN_LINE_HEIGHT = 9
REFINE_MAX_LINE_HEIGHT = 52
REFINE_MIN_ROI_HEIGHT = 20
REFINE_MAX_ROI_HEIGHT = 52
REFINE_LEFT_MARGIN = 4
# 精框找到的边界外再保留约半个字高，避免阈值变化把首尾汉字的外侧笔画削掉。
REFINE_EDGE_CONTEXT_DIVISOR = 2
# 检测文字边界（含首尾保护）达到该宽度时，说明标题已经横跨画面。
# 此时直接使用最大 312x52 横向带，杜绝连通域阈值继续削掉首尾字。
REFINE_FULL_WIDTH_TRIGGER = 210

# V5.0 文字行候选参数。先用局部自适应阈值形成二值图，再把字符连通域聚成行。
ADAPTIVE_MEAN_SIZE = 2
ADAPTIVE_OFFSET = 7
TEXT_MIN_COMPONENT_WIDTH = 2
TEXT_MIN_COMPONENT_HEIGHT = 4
TEXT_MAX_COMPONENT_WIDTH = 280
TEXT_MAX_COMPONENT_HEIGHT = 52
TEXT_MAX_COMPONENT_AREA = 14000
TEXT_LINE_MAX_PROPOSALS = 6
TEXT_LINE_MIN_COMPONENTS = 2
TEXT_LINE_MIN_SCORE = 500
OCR_GUIDED_MIN_RAW_LENGTH = 2
OCR_GUIDED_STRONG_DISTANCE = 1
OCR_GUIDED_LOCAL_SHIFT_DIVISOR = 3
# V6.3 不再创建缩放图；所有文字候选都直接从 RGB565 原帧的 LAB 亮度提取。
DETECT_PRIMARY_DIVISOR = 1
DETECT_FALLBACK_DIVISOR = 1

# 标题行限制。允许三个较小汉字组成的短药名。
TITLE_MIN_WIDTH = 42
TITLE_MIN_HEIGHT = 12
TITLE_MAX_HEIGHT = 72

# 浅色药盒面板限制。原来的高度110和面积45%对近距离药盒过严。
PANEL_MIN_WIDTH = 80
PANEL_MIN_HEIGHT = 20
PANEL_MAX_HEIGHT = 190
PANEL_MAX_AREA_PERCENT = 88

# 单帧最多处理的有效文字连通区域，防止噪点过多拖慢程序。
TEXT_COMPONENT_LIMIT = 128

# 曝光计量必须落在药盒白底/标题附近，不能让大面积黑背景主导 P75。
METER_ROI = (24, 84, 216, 44)
METER_TARGET_P75 = 135
METER_ACCEPT_LOW = 105
METER_ACCEPT_HIGH = 175
METER_HIGHLIGHT_LIMIT = 235
METER_MIN_RANGE = 30
EXPOSURE_MIN_US = 500
EXPOSURE_INITIAL_US = 2500
EXPOSURE_MAX_US = 5000
EXPOSURE_ADJUST_ROUNDS = 8
GAIN_INITIAL_DB = 0.0
GAIN_MIN_DB = 0.0
GAIN_MAX_DB = 2.0
RUN_ONCE = False


def first_existing_path(filename):
    for path in (
        "/sdcard/" + filename,
        "/sd/" + filename,
        filename,
    ):
        try:
            os.stat(path)
            return path
        except OSError:
            pass
    return None


def load_utf8_lines(path):
    values = []
    with open(path, "rb") as source_file:
        for raw_line in source_file:
            value = raw_line.decode("utf-8").rstrip("\r\n")
            if not values and value.startswith("\ufeff"):
                value = value[1:]
            if value:
                values.append(value)
    return values


def load_charset(path):
    charset = load_utf8_lines(path)
    if len(charset) != CHARSET_SIZE:
        raise ValueError(
            "字符表数量错误：%d，预期：%d"
            % (len(charset), CHARSET_SIZE)
        )
    for index, char in enumerate(charset):
        if len(char) != 1:
            raise ValueError("字符表第%d项不是单字符" % index)
    return charset


def sha256_file(path):
    """用小块读取计算 SHA-256；不把 290 KB 模型整体读入 Python 堆。"""
    try:
        import uhashlib as hash_module
    except ImportError:
        try:
            import hashlib as hash_module
        except ImportError:
            return None
    try:
        digest = hash_module.sha256()
    except TypeError:
        digest = hash_module.sha256(b"")
    with open(path, "rb") as source_file:
        while True:
            block = source_file.read(1024)
            if not block:
                break
            digest.update(block)
    return digest.digest()


def validate_deployment_files(model_path, charset_path, dictionary_path):
    """按内容拒绝同大小的旧模型、旧字符表和旧词典混用。"""
    try:
        model_size = int(os.stat(model_path)[6])
    except Exception as error:
        raise OSError("无法读取模型大小：%r" % (error,))
    if model_size != EXPECTED_MODEL_SIZE:
        raise ValueError(
            "模型文件不匹配：%d bytes，预期%d bytes"
            % (model_size, EXPECTED_MODEL_SIZE)
        )
    checks = (
        (model_path, EXPECTED_MODEL_SHA256, "模型"),
        (charset_path, EXPECTED_CHARSET_SHA256, "字符表"),
        (dictionary_path, EXPECTED_DRUG_NAMES_SHA256, "药名词典"),
    )
    hash_available = True
    for path, expected_digest, label in checks:
        actual_digest = sha256_file(path)
        if actual_digest is None:
            hash_available = False
            break
        if actual_digest != expected_digest:
            raise ValueError(
                "%s内容版本不匹配；请重新复制部署包中的同名文件"
                % label
            )
    if hash_available:
        print("[资源校验] 模型、字符表、药名词典 SHA-256 全部一致")
    else:
        print("[警告] 固件没有SHA-256模块，仅执行结构校验")


def validate_deployment_assets(charset, names):
    """再次检查运行时解析出的类别顺序和目标词典。"""
    for index, expected_char in EXPECTED_CHARSET_SENTINELS:
        if index >= len(charset) or charset[index] != expected_char:
            actual_char = "<越界>" if index >= len(charset) else charset[index]
            raise ValueError(
                "字符表顺序不匹配：索引%d为%s，预期%s"
                % (index, actual_char, expected_char)
            )
    if "枸橼酸莫沙必利片" not in names:
        raise ValueError("药名词典缺少：枸橼酸莫沙必利片")
    if "复方黄连素片" not in names:
        raise ValueError("药名词典缺少：复方黄连素片")
    print("[资源校验] 字符类别顺序和目标药名结构一致")


def load_drug_dictionary(path, charset):
    names = load_utf8_lines(path)
    if not names:
        raise ValueError("药名词典为空")
    if len(names) >= NO_CANDIDATE:
        raise ValueError("药名词典最多支持254项")

    codes = []
    seen = []
    max_length = 0
    for name in names:
        if name in seen:
            raise ValueError("药名词典存在重复项：" + name)
        seen.append(name)
        code = []
        for char in name:
            try:
                code.append(charset.index(char))
            except ValueError:
                raise ValueError("药名包含字符表外字符：" + name + " -> " + char)
        if len(code) < 3 or len(code) > TIME_STEPS:
            raise ValueError("药名长度不适合模型：" + name)
        codes.append(code)
        if len(code) > max_length:
            max_length = len(code)
    return names, codes, max_length


class DrugClassifierDecoder:
    """把 34 类概率转换为现有词典投票链路可复用的药名类别 ID。"""

    def __init__(self, codes):
        if len(codes) != DRUG_CLASS_COUNT:
            raise ValueError("分类模型要求%d个药名" % DRUG_CLASS_COUNT)
        self.codes = codes
        self.last_top_index = UNKNOWN_CLASS_INDEX
        self.last_confidence_milli = 0
        self.last_margin_milli = 0
        self.last_accepted = False

    def decode(self, values, output_ids):
        iterator = iter(values)
        best_index = -1
        best_value = -1.0
        second_value = -1.0
        for index in range(MODEL_OUTPUT_COUNT):
            try:
                value = float(next(iterator))
            except StopIteration:
                raise ValueError(
                    "分类模型输出不足，预期%d个值" % MODEL_OUTPUT_COUNT
                )
            if value > best_value:
                second_value = best_value
                best_value = value
                best_index = index
            elif value > second_value:
                second_value = value
        try:
            next(iterator)
            raise ValueError(
                "分类模型输出过长，预期%d个值" % MODEL_OUTPUT_COUNT
            )
        except StopIteration:
            pass

        margin = best_value - second_value
        self.last_top_index = best_index
        self.last_confidence_milli = int(best_value * 1000 + 0.5)
        self.last_margin_milli = int(margin * 1000 + 0.5)
        self.last_accepted = (
            best_index >= 0
            and best_index < DRUG_CLASS_COUNT
            and best_value >= CLASS_CONFIDENCE_MIN
            and margin >= CLASS_MARGIN_MIN
        )
        if not self.last_accepted:
            return 0
        code = self.codes[best_index]
        for index in range(len(code)):
            output_ids[index] = code[index]
        return len(code)


class DrugMatcher:
    """使用两个预分配 DP 行执行类别 ID 级编辑距离。"""

    def __init__(self, codes, max_length):
        self.codes = codes
        self.previous = bytearray(max_length + 1)
        self.current = bytearray(max_length + 1)
        self.last_best_index = NO_CANDIDATE
        self.last_best_distance = 99
        self.last_second_distance = 99
        self.last_prefix_match = False

    def distance(self, left, left_length, right):
        right_length = len(right)
        previous = self.previous
        current = self.current
        for index in range(right_length + 1):
            previous[index] = index

        for left_index in range(1, left_length + 1):
            current[0] = left_index
            left_value = left[left_index - 1]
            for right_index in range(1, right_length + 1):
                insertion = current[right_index - 1] + 1
                deletion = previous[right_index] + 1
                substitution = (
                    previous[right_index - 1]
                    + int(left_value != right[right_index - 1])
                )
                value = insertion
                if deletion < value:
                    value = deletion
                if substitution < value:
                    value = substitution
                current[right_index] = value
            previous, current = current, previous

        self.previous = previous
        self.current = current
        return previous[right_length]

    def match(self, raw_ids, raw_length):
        self.last_best_index = NO_CANDIDATE
        self.last_best_distance = 99
        self.last_second_distance = 99
        self.last_prefix_match = False
        if raw_length < 2:
            return NO_CANDIDATE, 0, 99

        best_index = NO_CANDIDATE
        best_distance = 99
        second_distance = 99
        best_length = 0
        for index, code in enumerate(self.codes):
            code_length = len(code)
            if abs(raw_length - code_length) > 2:
                continue
            if raw_length * 4 < code_length * 3:
                continue
            distance = self.distance(raw_ids, raw_length, code)
            if distance == 0:
                self.last_best_index = index
                self.last_best_distance = 0
                self.last_second_distance = second_distance
                return index, 3, 0
            if distance < best_distance:
                second_distance = best_distance
                best_distance = distance
                best_index = index
                best_length = code_length
            elif distance < second_distance:
                second_distance = distance

        if best_index != NO_CANDIDATE:
            self.last_best_index = best_index
            self.last_best_distance = best_distance
            self.last_second_distance = second_distance
            if best_distance == 1 and second_distance > 1:
                return best_index, 2, best_distance
            if (
                best_distance == 2
                and best_length >= 5
                and second_distance >= 4
                and best_distance * 10 <= best_length * 4
            ):
                return best_index, 1, best_distance
            # 真机字体、反光和边缘裁切可能带来3～4个字符错误。仅当长药名的
            # 最近候选明显领先次优候选，且错误比例不超过一半时给一张弱票；
            # 它仍需在7帧窗口累计6票才会成为稳定输出。
            if (
                best_distance >= 3
                and best_length >= 6
                and second_distance >= best_distance + 2
                and best_distance * 2 <= best_length
            ):
                return best_index, 1, best_distance

        # 真机日志表明长药名有时只解出开头2～5个字，其余时间步全部为
        # CTC blank。若前两个类别只对应一个长度至少6的白名单药名，给它
        # 一张中等权重票；仍需至少3个一致帧才会达到稳定阈值。
        prefix_index = NO_CANDIDATE
        for index, code in enumerate(self.codes):
            if (
                len(code) >= 6
                and raw_ids[0] == code[0]
                and raw_ids[1] == code[1]
            ):
                if prefix_index != NO_CANDIDATE:
                    prefix_index = NO_CANDIDATE
                    break
                prefix_index = index
        if prefix_index != NO_CANDIDATE:
            self.last_best_index = prefix_index
            self.last_best_distance = best_distance
            self.last_second_distance = second_distance
            self.last_prefix_match = True
            return prefix_index, 2, best_distance

        if best_index == NO_CANDIDATE:
            return NO_CANDIDATE, 0, 99
        return NO_CANDIDATE, 0, best_distance


def ids_to_text(values, length, charset):
    text = ""
    for index in range(length):
        text += charset[values[index]]
    return text


class WeightedVoter:
    """7 帧候选索引加权投票；255 表示本帧拒绝。"""

    def __init__(self, candidate_count):
        self.history_index = bytearray([NO_CANDIDATE] * HISTORY_SIZE)
        self.history_weight = bytearray(HISTORY_SIZE)
        self.scores = bytearray(candidate_count)
        self.position = 0
        self.stable = NO_CANDIDATE
        self.misses = 0

    def update(self, candidate_index, weight):
        old_index = self.history_index[self.position]
        old_weight = self.history_weight[self.position]
        if old_index != NO_CANDIDATE:
            if self.scores[old_index] < old_weight:
                raise RuntimeError("投票计数下溢")
            self.scores[old_index] -= old_weight

        self.history_index[self.position] = candidate_index
        self.history_weight[self.position] = weight
        if candidate_index != NO_CANDIDATE:
            self.scores[candidate_index] += weight
        self.position = (self.position + 1) % HISTORY_SIZE

        changed = False
        if self.stable != NO_CANDIDATE:
            if candidate_index == self.stable:
                self.misses = 0
            else:
                self.misses += 1
            if self.misses >= 3:
                self.stable = NO_CANDIDATE
                self.misses = 0
                changed = True

        top_index = NO_CANDIDATE
        top_score = -1
        second_score = -1
        for index, score in enumerate(self.scores):
            if score > top_score:
                second_score = top_score
                top_score = score
                top_index = index
            elif score > second_score:
                second_score = score

        margin = top_score - second_score
        if self.stable == NO_CANDIDATE:
            if (
                candidate_index == top_index
                and top_score >= 6
                and margin >= 2
            ):
                self.stable = top_index
                changed = True
        elif (
            top_index != self.stable
            and candidate_index == top_index
            and top_score >= 8
            and margin >= 3
        ):
            self.stable = top_index
            self.misses = 0
            changed = True

        return self.stable, changed, top_score, margin


def clamp(value, minimum, maximum):
    if value < minimum:
        return minimum
    if value > maximum:
        return maximum
    return value


def lab_l_to_gray(value):
    """把 RGB565 的 LAB L(0..100) 映射到原定位器使用的 0..255 尺度。"""
    return clamp((int(value) * 255 + 50) // 100, 0, 255)


def gray_to_lab_l(value):
    """把原灰度阈值映射为 RGB565 find_blobs() 所需的 LAB L。"""
    return clamp((int(value) * 100 + 127) // 255, 0, 100)


def dark_pixel_threshold(gray_limit):
    """返回与当前采集格式匹配的深色文字阈值。"""
    if COLOR_CAPTURE:
        return (0, gray_to_lab_l(gray_limit), -128, 127, -128, 127)
    return (0, int(gray_limit))


def luminance_statistics(image, roi=None):
    """统一返回 0..255 尺度的亮度均值、标准差、最小值和最大值。"""
    if roi is None:
        statistics = image.get_statistics()
    else:
        statistics = image.get_statistics(roi=roi)
    if COLOR_CAPTURE:
        return (
            lab_l_to_gray(statistics.l_mean()),
            lab_l_to_gray(statistics.l_stdev()),
            lab_l_to_gray(statistics.l_min()),
            lab_l_to_gray(statistics.l_max()),
        )
    return (
        int(statistics.mean()),
        int(statistics.stdev()),
        int(statistics.min()),
        int(statistics.max()),
    )


def luminance_histogram(image, roi, bins):
    """创建低内存亮度直方图；RGB565 只细分 L，A/B 使用最少有效桶。"""
    if COLOR_CAPTURE:
        try:
            return image.get_histogram(
                roi=roi,
                l_bins=bins,
                a_bins=4,
                b_bins=4,
            )
        except TypeError:
            # 兼容部分 RA8D1 厂商固件的旧参数表。
            return image.get_histogram(roi=roi, bins=bins)
    return image.get_histogram(roi=roi, bins=bins)


def luminance_histogram_value(value):
    """读取 percentile/threshold 的第一个通道并还原到 0..255。"""
    raw_value = int(value[0])
    if COLOR_CAPTURE:
        return lab_l_to_gray(raw_value)
    return raw_value


def ratio_roi_from_center(center_x, center_y, roi_height):
    """生成严格 6:1 的 OCR 框，并把它限制在 QVGA 画面内。"""
    ratio = IMAGE_WIDTH // IMAGE_HEIGHT
    roi_width = roi_height * ratio
    x = center_x - roi_width // 2
    y = center_y - roi_height // 2
    x = clamp(x, 2, FRAME_WIDTH - roi_width - 2)
    y = clamp(y, 2, FRAME_HEIGHT - roi_height - 2)
    return (x, y, roi_width, roi_height)


def sanitize_roi(roi):
    """把 ROI 转成有效整数矩形；无效矩形返回 None。"""
    try:
        x = int(roi[0])
        y = int(roi[1])
        width = int(roi[2])
        height = int(roi[3])
    except Exception:
        return None

    if width < 8 or height < 4:
        return None

    left = clamp(x, 0, FRAME_WIDTH - 2)
    top = clamp(y, 0, FRAME_HEIGHT - 2)
    right = clamp(x + width, left + 1, FRAME_WIDTH)
    bottom = clamp(y + height, top + 1, FRAME_HEIGHT)
    width = right - left
    height = bottom - top

    if width < 8 or height < 4:
        return None
    return (left, top, width, height)


def fit_title_roi(rect):
    """把紧文字框扩展成含背景留白、且与 192x32 同比例的框。"""
    x, y, width, height = rect
    # 训练图中正文通常不超过 168x25。扩框后再缩放，使实拍正文也大致
    # 落在同一区间，避免短药名被放得过大或长药名贴住左右边缘。
    height_for_text_height = (height * IMAGE_HEIGHT + 24) // 25
    height_for_text_width = (width * 4 + 20) // 21
    roi_height = height_for_text_height
    if height_for_text_width > roi_height:
        roi_height = height_for_text_width
    if roi_height < 20:
        roi_height = 20

    ratio = IMAGE_WIDTH // IMAGE_HEIGHT
    max_height = (FRAME_WIDTH - 4) // ratio
    if FRAME_HEIGHT - 4 < max_height:
        max_height = FRAME_HEIGHT - 4
    if roi_height > max_height:
        roi_height = max_height

    center_x = x + width // 2
    center_y = y + height // 2
    roi = ratio_roi_from_center(center_x, center_y, roi_height)
    # 若标题本身已经触边或大于可用框，拒绝该检测，不截断文字送入模型。
    if (
        x < roi[0]
        or y < roi[1]
        or x + width > roi[0] + roi[2]
        or y + height > roi[1] + roi[3]
    ):
        return None
    return roi


def fit_tight_title_roi(rect, alignment=1):
    """把大字真实边界变成小留白 6:1 精框，让文字尽量占满模型输入。"""
    x, y, width, height = rect

    # 连通域阈值经常只覆盖汉字主体，首尾最浅的笔画可能比检测边界多出数像素。
    # 在不超过最大 312px 的前提下给两端补少量上下文；默认对称分配，触边时
    # 把剩余空间让给另一侧。这里不复制图像，只改变最终 OCR ROI。
    edge_context = max(3, height // REFINE_EDGE_CONTEXT_DIVISOR)
    extra_budget = max(0, REFINE_MAX_ROI_HEIGHT * 6 - width)
    left_context = min(edge_context, max(0, x - 2), extra_budget // 2)
    right_context = min(
        edge_context,
        max(0, FRAME_WIDTH - 2 - (x + width)),
        extra_budget - left_context,
    )
    x -= left_context
    width += left_context + right_context

    # 对横跨画面的长药名（例如八、九个大字），精确的连通域边界会随阈值
    # 抖动并反复削掉首字或末字。达到阈值后不再继续猜横向端点，直接采用
    # 最大 OCR 横向带；仅保留检测得到的纵向中心，因此不会把下方英文行带入。
    if width >= REFINE_FULL_WIDTH_TRIGGER and height <= REFINE_MAX_ROI_HEIGHT:
        roi_height = REFINE_MAX_ROI_HEIGHT
        roi_width = roi_height * 6
        center_x = x + width // 2
        center_y = y + height // 2
        roi_x = clamp(
            center_x - roi_width // 2,
            2,
            FRAME_WIDTH - roi_width - 2,
        )
        roi_y = clamp(
            center_y - roi_height // 2,
            2,
            FRAME_HEIGHT - roi_height - 2,
        )
        full_width_roi = (roi_x, roi_y, roi_width, roi_height)
        if (
            x >= roi_x
            and y >= roi_y
            and x + width <= roi_x + roi_width
            and y + height <= roi_y + roi_height
        ):
            return full_width_roi

    # 普通标题保留左右各约4像素、上下各约2像素；接近画面极限的大标题
    # 自动压缩安全边距，优先保证所有汉字完整进入最大312x52输入框。
    horizontal_padding = 8
    if width > 288:
        horizontal_padding = 4
    if width > 304:
        horizontal_padding = 0
    vertical_padding = 4
    if height > 44:
        vertical_padding = 2
    if height > 50:
        vertical_padding = 0
    height_for_width = (width + horizontal_padding + 5) // 6
    height_for_text = height + vertical_padding
    roi_height = max(
        REFINE_MIN_ROI_HEIGHT,
        height_for_width,
        height_for_text,
    )
    if roi_height > REFINE_MAX_ROI_HEIGHT:
        return None

    # 药盒标题左侧经常紧邻品牌图标。精框按文字左沿对齐，把 6:1 比例
    # 产生的多余空间放到右侧，避免居中扩框又把左侧商标收回来。
    roi_width = roi_height * 6
    if alignment < 0:
        roi_x = x + width + REFINE_LEFT_MARGIN - roi_width
    elif alignment > 0:
        roi_x = x - REFINE_LEFT_MARGIN
    else:
        roi_x = x + width // 2 - roi_width // 2
    roi_x = clamp(roi_x, 2, FRAME_WIDTH - roi_width - 2)

    # 超宽标题可能已经没有空间保留左侧品牌安全边距。若偏置后的框会截掉
    # 任一端文字，立即改为围绕真实文字边界居中，最大 312px 宽度也能完整容纳。
    if x < roi_x or x + width > roi_x + roi_width:
        centered_x = x + width // 2 - roi_width // 2
        roi_x = clamp(centered_x, 2, FRAME_WIDTH - roi_width - 2)
    center_y = y + height // 2
    roi_y = clamp(
        center_y - roi_height // 2,
        2,
        FRAME_HEIGHT - roi_height - 2,
    )
    roi = (roi_x, roi_y, roi_width, roi_height)
    if (
        x < roi[0]
        or y < roi[1]
        or x + width > roi[0] + roi[2]
        or y + height > roi[1] + roi[3]
    ):
        return None
    return roi


def smooth_title_roi(old_roi, new_roi):
    old_center_x = old_roi[0] + old_roi[2] // 2
    old_center_y = old_roi[1] + old_roi[3] // 2
    new_center_x = new_roi[0] + new_roi[2] // 2
    new_center_y = new_roi[1] + new_roi[3] // 2
    center_x = (old_center_x * 3 + new_center_x + 2) // 4
    center_y = (old_center_y * 3 + new_center_y + 2) // 4
    roi_height = (old_roi[3] * 3 + new_roi[3] + 2) // 4
    return ratio_roi_from_center(center_x, center_y, roi_height)


def roi_iou_percent(first, second):
    """返回两个矩形的整数 IoU 百分比，用于拒绝单帧误框。"""
    left = max(first[0], second[0])
    top = max(first[1], second[1])
    right = min(first[0] + first[2], second[0] + second[2])
    bottom = min(first[1] + first[3], second[1] + second[3])
    if right <= left or bottom <= top:
        return 0
    intersection = (right - left) * (bottom - top)
    union = first[2] * first[3] + second[2] * second[3] - intersection
    if union <= 0:
        return 0
    return (intersection * 100) // union


def expand_roi(roi, margin_x, margin_y):
    """在不越出 QVGA 画面的情况下扩展 ROI。"""
    x, y, width, height = roi
    left = max(2, x - margin_x)
    top = max(2, y - margin_y)
    right = min(FRAME_WIDTH - 2, x + width + margin_x)
    bottom = min(FRAME_HEIGHT - 2, y + height + margin_y)
    return (left, top, right - left, bottom - top)


def title_rois_compatible(first, second, minimum_iou):
    """判断两个 6:1 标题框是否来自同一行文字。"""
    if roi_iou_percent(first, second) >= minimum_iou:
        return True

    first_center_x = first[0] + first[2] // 2
    first_center_y = first[1] + first[3] // 2
    second_center_x = second[0] + second[2] // 2
    second_center_y = second[1] + second[3] // 2

    maximum_height = max(first[3], second[3])
    minimum_height = min(first[3], second[3])

    if minimum_height <= 0:
        return False
    if abs(first_center_x - second_center_x) > maximum_height * 2:
        return False
    if abs(first_center_y - second_center_y) > maximum_height:
        return False
    if maximum_height * 100 > minimum_height * 160:
        return False
    return True


class TitleLocator:
    """自适应二值化 + 连通域文字行 + OCR 识别反馈定位器。

    传统 OCR 的成熟流程不是先猜“白色药盒面板”，而是先把局部文字从
    光照背景中分离，再生成若干文字行候选，最后用识别器确认候选。
    """

    def __init__(self, engine, matcher, codes, classifier_decoder):
        self.engine = engine
        self.matcher = matcher
        self.codes = codes
        self.classifier_decoder = classifier_decoder
        self.roi = DEFAULT_TITLE_ROI
        self.automatic = False
        self.available = AUTO_TITLE_ROI
        self.missed_frames = 0
        self.pending_roi = None
        self.pending_hits = 0

        self.last_source = "引导"
        self.last_line_score = -1
        self.last_probe_score = -99999
        self.last_probe_count = 0
        self.last_raw_length = 0
        self.last_best_distance = 99
        self.last_threshold = ADAPTIVE_OFFSET
        self.panel_roi = TITLE_SEARCH_ROI
        self.dark_limit = ADAPTIVE_OFFSET
        self.panel_limit = 0

        self.probe_ids = [0] * TIME_STEPS
        self.best_ids = [0] * TIME_STEPS
        self.last_detector_divisor = 0
        self.memory_warning_shown = False
        self.last_dark_limit = 0
        self.last_refine_components = 0
        self.last_refine_dark_limit = 0

    @staticmethod
    def _sort_components_by_x(components):
        # 列表很短，插入排序比引入 key/lambda 更节省 MicroPython 堆内存。
        index = 1
        while index < len(components):
            value = components[index]
            position = index - 1
            while position >= 0 and components[position][0] > value[0]:
                components[position + 1] = components[position]
                position -= 1
            components[position + 1] = value
            index += 1

    @staticmethod
    def _sort_proposals_by_score(proposals):
        """按视觉分数降序排列；避免 key/lambda 产生额外堆对象。"""
        first = 0
        while first < len(proposals):
            best = first
            second = first + 1
            while second < len(proposals):
                if proposals[second][1] > proposals[best][1]:
                    best = second
                second += 1
            if best != first:
                proposals[first], proposals[best] = (
                    proposals[best],
                    proposals[first],
                )
            first += 1

    def _roi_visual_score(self, image, roi, position_score):
        """用低成本亮度统计判断窗口中是否存在浅底深色的大字行。"""
        try:
            mean, stdev, minimum, maximum = luminance_statistics(image, roi)
            contrast = maximum - minimum
            score = position_score + stdev * 36 + contrast * 8
            # 全黑背景、纯白空白和低对比区域不是药名标题。
            if 55 <= mean <= 225:
                score += 400
            else:
                score -= 700
            if stdev < 16:
                score -= 1200
            return score
        except Exception:
            return position_score

    @staticmethod
    def _fine_component_compatible(line, component):
        """合并同一标题行的汉字笔画，同时拒绝明显分离的其他文字行。"""
        line_height = line[3] - line[1]
        component_height = component[3] - component[1]
        maximum_height = max(line_height, component_height)
        minimum_height = min(line_height, component_height)
        if minimum_height <= 0:
            return False, -10000
        # find_blobs(merge=False) 返回的是笔画块而不是完整汉字；点、撇等短笔画
        # 可能只有主体高度的约三分之一，不能按完整字符的高度比例过滤。
        if maximum_height * 100 > minimum_height * 360:
            return False, -10000

        line_center_y = (line[1] + line[3]) // 2
        component_center_y = (component[1] + component[3]) // 2
        center_difference = abs(line_center_y - component_center_y)
        if center_difference > max(4, maximum_height * 3 // 5):
            return False, -10000

        horizontal_gap = max(
            component[0] - line[2],
            line[0] - component[2],
            0,
        )
        # 允许跨过汉字内部的断笔和正常字间距；真正的商标分隔交给后面的
        # 大间隔判定，避免把首尾汉字拆成另一条“行”。
        if horizontal_gap > max(10, maximum_height * 3 // 4):
            return False, -10000
        return True, -center_difference * 10 - horizontal_gap * 2

    @staticmethod
    def _recover_title_edges(components, line):
        """从同一基线的邻近笔画中找回被拆开的首尾汉字。"""
        left, top, right, bottom, pixels, count = line
        base_top = top
        base_bottom = bottom
        base_height = max(1, bottom - top)
        base_center_y = (top + bottom) // 2
        maximum_reach = max(18, base_height * 3 // 2)
        minimum_x = max(2, left - maximum_reach)
        maximum_x = min(FRAME_WIDTH - 2, right + maximum_reach)
        maximum_gap = max(10, base_height * 3 // 4)

        # 两轮足够让同一边缘汉字的多个笔画逐步桥接回来，同时把搜索距离限制
        # 在约一个半字宽内，避免沿整行粘到远处品牌和剂量文字。
        recovery_round = 0
        while recovery_round < 2:
            changed = False
            for component in components:
                component_left = component[0]
                component_top = component[1]
                component_right = component[2]
                component_bottom = component[3]
                if component_right < minimum_x or component_left > maximum_x:
                    continue
                if component_left >= left and component_right <= right:
                    continue

                component_height = component_bottom - component_top
                if component_height <= 0:
                    continue
                overlap = min(base_bottom, component_bottom) - max(
                    base_top,
                    component_top,
                )
                component_center_y = (component_top + component_bottom) // 2
                center_difference = abs(component_center_y - base_center_y)
                minimum_height = min(base_height, component_height)
                vertical_ok = (
                    overlap > 0
                    and overlap * 100 >= minimum_height * 18
                )
                if not vertical_ok:
                    vertical_ok = center_difference <= max(
                        4,
                        base_height * 3 // 5,
                    )
                if not vertical_ok:
                    continue

                horizontal_gap = max(
                    component_left - right,
                    left - component_right,
                    0,
                )
                if horizontal_gap > maximum_gap:
                    continue

                left = min(left, component_left)
                top = min(top, component_top)
                right = max(right, component_right)
                bottom = max(bottom, component_bottom)
                pixels += component[4]
                count += 1
                changed = True
            if not changed:
                break
            recovery_round += 1

        return [left, top, right, bottom, pixels, count]

    @staticmethod
    def _trim_edge_graphic(components, line):
        """从同一行中剥离与正文隔着明显空隙的左/右商标图形。"""
        selected = []
        line_height = line[3] - line[1]
        for component in components:
            if component[0] < line[0] or component[2] > line[2]:
                continue
            component_height = component[3] - component[1]
            minimum_height = min(line_height, component_height)
            overlap = min(line[3], component[3]) - max(line[1], component[1])
            if minimum_height > 0 and overlap * 100 >= minimum_height * 35:
                selected.append(component)

        if len(selected) < 3:
            del selected
            return line, 0

        largest_gap = -1
        second_gap = -1
        split_index = -1
        index = 1
        while index < len(selected):
            gap = selected[index][0] - selected[index - 1][2]
            if gap > largest_gap:
                second_gap = largest_gap
                largest_gap = gap
                split_index = index
            elif gap > second_gap:
                second_gap = gap
            index += 1

        if (
            split_index < 1
            or largest_gap < max(14, line_height * 3 // 4)
            or largest_gap < second_gap + 5
        ):
            del selected
            return line, 0

        left_left = selected[0][0]
        left_top = selected[0][1]
        left_right = selected[0][2]
        left_bottom = selected[0][3]
        left_pixels = selected[0][4]
        index = 1
        while index < split_index:
            component = selected[index]
            left_top = min(left_top, component[1])
            left_right = max(left_right, component[2])
            left_bottom = max(left_bottom, component[3])
            left_pixels += component[4]
            index += 1

        right = selected[split_index]
        right_left = right[0]
        right_top = right[1]
        right_right = right[2]
        right_bottom = right[3]
        right_pixels = right[4]
        index = split_index + 1
        while index < len(selected):
            component = selected[index]
            right_top = min(right_top, component[1])
            right_right = max(right_right, component[2])
            right_bottom = max(right_bottom, component[3])
            right_pixels += component[4]
            index += 1

        left_width = left_right - left_left
        right_width = right_right - right_left
        left_count = split_index
        right_count = len(selected) - split_index
        del selected

        left_is_graphic = (
            left_width * 10 <= line_height * 15
            and right_width >= line_height * 3
            and left_count * 2 <= right_count + 1
        )
        right_is_graphic = (
            right_width * 10 <= line_height * 15
            and left_width >= line_height * 3
            and right_count * 2 <= left_count + 1
        )
        if left_is_graphic and not right_is_graphic:
            return (
                [
                    right_left,
                    right_top,
                    right_right,
                    right_bottom,
                    right_pixels,
                    right_count,
                ],
                1,
            )
        if right_is_graphic and not left_is_graphic:
            return (
                [
                    left_left,
                    left_top,
                    left_right,
                    left_bottom,
                    left_pixels,
                    left_count,
                ],
                -1,
            )
        return line, 0

    def _refine_title_roi(self, image, coarse_roi):
        """在粗框附近合并同一基线的大字符，返回紧贴标题的 6:1 ROI。"""
        search_roi = expand_roi(
            coarse_roi,
            REFINE_MARGIN_X,
            REFINE_MARGIN_Y,
        )
        if REFINE_FULL_WIDTH_SEARCH:
            # 大标题可能接近画面两侧。即使上一帧误锁到半截文字，本帧也从
            # 同一纵向带的完整宽度重新合并字符，避免局部跟踪越跟越窄。
            search_roi = (
                2,
                search_roi[1],
                FRAME_WIDTH - 4,
                search_roi[3],
            )
        blobs = None
        components = []
        lines = []
        try:
            histogram = luminance_histogram(image, search_roi, 32)
            threshold = luminance_histogram_value(histogram.get_threshold())
            p50 = luminance_histogram_value(histogram.get_percentile(0.50))
            p75 = luminance_histogram_value(histogram.get_percentile(0.75))
            del histogram

            dark_limit = min(threshold + 8, p50 - 8, p75 - 18)
            dark_limit = clamp(dark_limit, 35, 175)
            self.last_refine_dark_limit = dark_limit

            blobs = image.find_blobs(
                [dark_pixel_threshold(dark_limit)],
                roi=search_roi,
                x_stride=1,
                y_stride=1,
                pixels_threshold=3,
                area_threshold=4,
                merge=False,
            )

            coarse_center_y = coarse_roi[1] + coarse_roi[3] // 2
            vertical_tolerance = coarse_roi[3] // 2 + REFINE_MARGIN_Y
            for blob in blobs:
                x = int(blob[0])
                y = int(blob[1])
                width = int(blob[2])
                height = int(blob[3])
                pixels = int(blob[4])
                area = width * height
                center_y = y + height // 2

                if (
                    width < REFINE_MIN_COMPONENT_WIDTH
                    or height < REFINE_MIN_COMPONENT_HEIGHT
                    or width > REFINE_MAX_COMPONENT_WIDTH
                    or height > REFINE_MAX_COMPONENT_HEIGHT
                    or abs(center_y - coarse_center_y) > vertical_tolerance
                ):
                    continue
                if width > height * 10 and height < 10:
                    continue
                if height > width * 10 and width < 5:
                    continue
                if pixels * 100 < area * 2 or pixels * 100 > area * 94:
                    continue

                components.append([x, y, x + width, y + height, pixels])
                if len(components) >= REFINE_COMPONENT_LIMIT:
                    break

            self.last_refine_components = len(components)
            if not components:
                return None
            self._sort_components_by_x(components)

            # 每行：[left, top, right, bottom, pixels, component_count]
            for component in components:
                best_index = -1
                best_score = -10000
                for line_index, line in enumerate(lines):
                    compatible, score = self._fine_component_compatible(
                        line,
                        component,
                    )
                    if compatible and score > best_score:
                        best_index = line_index
                        best_score = score

                if best_index < 0:
                    lines.append(
                        [
                            component[0],
                            component[1],
                            component[2],
                            component[3],
                            component[4],
                            1,
                        ]
                    )
                else:
                    line = lines[best_index]
                    line[0] = min(line[0], component[0])
                    line[1] = min(line[1], component[1])
                    line[2] = max(line[2], component[2])
                    line[3] = max(line[3], component[3])
                    line[4] += component[4]
                    line[5] += 1

            best_roi = None
            best_score = -100000
            for line in lines:
                line = self._recover_title_edges(components, line)
                line, alignment = self._trim_edge_graphic(components, line)
                left, top, right, bottom, pixels, count = line
                width = right - left
                height = bottom - top
                area = width * height
                if (
                    width < REFINE_MIN_LINE_WIDTH
                    or height < REFINE_MIN_LINE_HEIGHT
                    or height > REFINE_MAX_LINE_HEIGHT
                    or width * 10 < height * 18
                ):
                    continue
                if count < 2 and width < height * 3:
                    continue
                if pixels * 100 < area * 2:
                    continue

                roi = fit_tight_title_roi(
                    (left, top, width, height),
                    alignment,
                )
                if roi is None:
                    continue
                line_center_y = top + height // 2
                score = (
                    height * height * 80
                    + width * height * 2
                    + count * 260
                    + pixels
                    - abs(line_center_y - coarse_center_y) * 80
                )
                if score > best_score:
                    best_score = score
                    best_roi = roi
            return best_roi
        except Exception as error:
            if not self.memory_warning_shown:
                print("[提示] 标题精框失败，暂用粗框：", repr(error))
                self.memory_warning_shown = True
            return None
        finally:
            if blobs is not None:
                del blobs
            del components
            del lines

    @staticmethod
    def _component_line_compatible(line, component):
        line_height = line[3] - line[1]
        component_height = component[3] - component[1]
        maximum_height = max(line_height, component_height)
        minimum_height = min(line_height, component_height)
        if minimum_height <= 0:
            return False, -10000

        # 大字标题和紧邻的小字说明通常高度差很大，先阻止它们被合成一行。
        if maximum_height * 100 > minimum_height * 260:
            return False, -10000

        overlap = min(line[3], component[3]) - max(line[1], component[1])
        line_center_y = (line[1] + line[3]) // 2
        component_center_y = (component[1] + component[3]) // 2
        center_difference = abs(line_center_y - component_center_y)

        vertical_ok = (
            overlap > 0
            and overlap * 100 >= minimum_height * 22
        )
        if not vertical_ok:
            vertical_ok = center_difference <= max(3, maximum_height * 2 // 5)
        if not vertical_ok:
            return False, -10000

        horizontal_gap = max(
            component[0] - line[2],
            line[0] - component[2],
            0,
        )
        if horizontal_gap > max(14, maximum_height * 3):
            return False, -10000

        score = (
            max(0, overlap) * 10
            - center_difference * 8
            - horizontal_gap
        )
        return True, score

    @staticmethod
    def _proposal_duplicate(proposals, roi):
        for existing in proposals:
            if roi_iou_percent(existing[0], roi) >= 90:
                return True
        return False

    def _add_proposal(self, proposals, roi, visual_score, source):
        if roi is None:
            return
        roi = sanitize_roi(roi)
        if roi is None:
            return
        if self._proposal_duplicate(proposals, roi):
            return
        proposals.append([roi, visual_score, source])

    def _estimate_direct_dark_limit(self, image):
        """在原始彩色帧的亮度通道上估计深色文字阈值，不创建副本。"""
        try:
            histogram = luminance_histogram(image, TITLE_SEARCH_ROI, 64)
            threshold = luminance_histogram_value(histogram.get_threshold())
            p50 = luminance_histogram_value(histogram.get_percentile(0.50))
            p75 = luminance_histogram_value(histogram.get_percentile(0.75))
            del histogram

            # 药盒标题通常为浅底深字。阈值不能高到吞掉灰色边框，
            # 也不能低到只剩少量最黑笔画。
            dark_limit = min(
                threshold + 12,
                p50 - 5,
                p75 - 14,
            )
            return clamp(dark_limit, 45, 170)
        except Exception:
            mean, stdev, minimum, maximum = luminance_statistics(
                image,
                TITLE_SEARCH_ROI,
            )
            return clamp(mean - max(12, stdev // 2), 45, 165)

    def _generate_text_line_proposals_direct(self, image):
        """直接在原始 QVGA RGB565 帧的 LAB 亮度通道上提取文字行。

        这里故意不调用 image.copy(..., x_scale=..., y_scale=...)。
        OpenMV 4.5.0 的部分 RA8D1 固件在 ROI 与缩放同时使用时会产生
        Invalid ROI dimensions，甚至把 IDE 帧缓冲错误地显示为 2x2。
        """
        proposals = []
        blobs = None
        components = []
        lines = []

        try:
            dark_limit = self._estimate_direct_dark_limit(image)
            self.last_dark_limit = dark_limit
            self.dark_limit = dark_limit

            blobs = image.find_blobs(
                [dark_pixel_threshold(dark_limit)],
                roi=TITLE_SEARCH_ROI,
                x_stride=1,
                y_stride=1,
                pixels_threshold=3,
                area_threshold=4,
                merge=True,
                margin=1,
            )

            for blob in blobs:
                x = int(blob[0])
                y = int(blob[1])
                width = int(blob[2])
                height = int(blob[3])
                pixels = int(blob[4])
                area = width * height

                if (
                    width < TEXT_MIN_COMPONENT_WIDTH
                    or height < TEXT_MIN_COMPONENT_HEIGHT
                    or width > TEXT_MAX_COMPONENT_WIDTH
                    or height > TEXT_MAX_COMPONENT_HEIGHT
                    or area > TEXT_MAX_COMPONENT_AREA
                ):
                    continue

                # 排除药盒横边、竖边、大片阴影和近似实心图案。
                if width > height * 12 and height < 9:
                    continue
                if height > width * 12 and width < 5:
                    continue
                if pixels * 100 < area * 2:
                    continue
                if pixels * 100 > area * 94:
                    continue

                components.append(
                    [x, y, x + width, y + height, pixels]
                )
                if len(components) >= TEXT_COMPONENT_LIMIT:
                    break

            if not components:
                self.last_detector_divisor = 1
                return proposals

            self._sort_components_by_x(components)

            # 每行：[left, top, right, bottom, pixels, component_count]
            for component in components:
                best_index = -1
                best_score = -10000
                for line_index, line in enumerate(lines):
                    compatible, score = self._component_line_compatible(
                        line,
                        component,
                    )
                    if compatible and score > best_score:
                        best_score = score
                        best_index = line_index

                if best_index < 0:
                    lines.append(
                        [
                            component[0],
                            component[1],
                            component[2],
                            component[3],
                            component[4],
                            1,
                        ]
                    )
                else:
                    line = lines[best_index]
                    line[0] = min(line[0], component[0])
                    line[1] = min(line[1], component[1])
                    line[2] = max(line[2], component[2])
                    line[3] = max(line[3], component[3])
                    line[4] += component[4]
                    line[5] += 1

            for line in lines:
                left, top, right, bottom, pixels, component_count = line
                width = right - left
                height = bottom - top
                area = width * height

                if (
                    width < TITLE_MIN_WIDTH
                    or height < TITLE_MIN_HEIGHT
                    or height > TITLE_MAX_HEIGHT
                    or width * 10 < height * 13
                ):
                    continue

                if (
                    component_count < TEXT_LINE_MIN_COMPONENTS
                    and width < height * 3
                ):
                    continue

                # 说明小字/英文行通常高度低而且横向很长。
                if height < 13 and width > height * 5:
                    continue
                if pixels * 100 < area * 2:
                    continue

                roi = fit_title_roi((left, top, width, height))
                if roi is None:
                    continue

                center_y = top + height // 2
                default_center_y = (
                    DEFAULT_TITLE_ROI[1]
                    + DEFAULT_TITLE_ROI[3] // 2
                )

                visual_score = (
                    height * height * 26
                    + width * (height + 10)
                    + component_count * 170
                    + pixels
                    - abs(center_y - default_center_y) * 35
                )

                self._add_proposal(
                    proposals,
                    roi,
                    visual_score,
                    "原图连通域行",
                )

            self.last_detector_divisor = 1

        finally:
            if blobs is not None:
                del blobs
            del components
            del lines
            gc.collect()

        # 视觉分数降序，避免 lambda 产生额外临时对象。
        first = 0
        while first < len(proposals):
            best = first
            second = first + 1
            while second < len(proposals):
                if proposals[second][1] > proposals[best][1]:
                    best = second
                second += 1
            if best != first:
                proposals[first], proposals[best] = (
                    proposals[best],
                    proposals[first],
                )
            first += 1

        if len(proposals) > TEXT_LINE_MAX_PROPOSALS:
            del proposals[TEXT_LINE_MAX_PROPOSALS:]
        return proposals

    def _generate_text_line_proposals(self, image):
        """低内存原图连通域检测；失败时由 OCR 滑窗继续工作。"""
        try:
            return self._generate_text_line_proposals_direct(image)
        except Exception as error:
            print(
                "[提示] 原图文字行候选生成失败，使用OCR滑窗：",
                repr(error),
            )
            self.last_detector_divisor = 1
            return []

    def _add_tracking_proposals(self, image, proposals):
        """在当前框附近增加多尺度候选，修正轻微移动和框大小误差。"""
        x, y, width, height = self.roi
        center_x = x + width // 2
        center_y = y + height // 2
        shift = max(6, height // OCR_GUIDED_LOCAL_SHIFT_DIVISOR)

        for dy in (-shift, 0, shift):
            roi = ratio_roi_from_center(center_x, center_y + dy, height)
            if roi is not None:
                base_score = 900 if dy == 0 else 700
                visual_score = self._roi_visual_score(
                    image,
                    roi,
                    base_score,
                )
                self._add_proposal(proposals, roi, visual_score, "跟踪")

        small_height = max(24, height * 4 // 5)
        large_height = min(52, height * 6 // 5)
        small_roi = ratio_roi_from_center(
            center_x,
            center_y,
            small_height,
        )
        large_roi = ratio_roi_from_center(
            center_x,
            center_y,
            large_height,
        )
        if small_roi is not None:
            self._add_proposal(
                proposals,
                small_roi,
                self._roi_visual_score(image, small_roi, 650),
                "缩小跟踪",
            )
        if large_roi is not None:
            self._add_proposal(
                proposals,
                large_roi,
                self._roi_visual_score(image, large_roi, 650),
                "放大跟踪",
            )

    def _add_grid_fallbacks(self, image, proposals):
        """沿画面中上部生成窗口；先做亮度排序，再让少量窗口进入 OCR。

        以前的三个候选互相重叠超过 62%，会被去重成一个候选，所以日志始终
        显示“候选：1”。本版保留七个不同高度位置，让识别器真正比较药名行。
        """
        # 当前固定机位的大标题位于画面上部。下方 y>=76 主要是说明小字，
        # 不再纳入全局窗口，避免高密度段落把框从药名上拖走。
        scan_tops = (20, 28, 36, 44, 52, 60, 68)
        target_center_y = 64
        for top in scan_tops:
            roi = sanitize_roi((40, top, 240, 40))
            if roi is None:
                continue
            center_y = top + 20
            position_score = 2200 - abs(center_y - target_center_y) * 30
            visual_score = self._roi_visual_score(
                image,
                roi,
                position_score,
            )
            self._add_proposal(
                proposals,
                roi,
                visual_score,
                "亮度预选+OCR",
            )
            if len(proposals) >= GLOBAL_VISUAL_PROPOSAL_LIMIT:
                break
        self.last_detector_divisor = 0

    def _recognition_score(self, raw_length, candidate, weight, visual_score):
        if raw_length < OCR_GUIDED_MIN_RAW_LENGTH:
            # 最终拒识时仍保留“最高目标类别”的定位信息。这样正确标题即使
            # 暂时只有六七成置信度，也能在候选排序中压过未知类和说明小字；
            # 该提示只决定框的位置，不会绕过最终分类门槛进入投票。
            if self._classification_location_hint(False):
                return (
                    visual_score // 60
                    + self.classifier_decoder.last_confidence_milli * 30
                    + self.classifier_decoder.last_margin_milli * 15
                    + 3000
                )
            return -100000 + visual_score // 100

        best_index = self.matcher.last_best_index
        best_distance = self.matcher.last_best_distance
        second_distance = self.matcher.last_second_distance

        if best_index == NO_CANDIDATE:
            return raw_length * 60 + visual_score // 80 - 9000

        code_length = len(self.codes[best_index])
        length_difference = abs(raw_length - code_length)
        separation = max(0, second_distance - best_distance)

        score = (
            visual_score // 60
            + raw_length * 180
            - best_distance * 1600
            - length_difference * 550
            + separation * 260
        )
        if candidate != NO_CANDIDATE:
            score += 7000 + weight * 6000
            # 闭集分类器的置信度和前两类差值比旧编辑距离更能区分
            # “真正药名行”和“说明小字碰巧像某个类别”的窗口。
            score += self.classifier_decoder.last_confidence_milli * 16
            score += self.classifier_decoder.last_margin_milli * 8
        if best_distance <= OCR_GUIDED_STRONG_DISTANCE:
            score += 5000
        return score

    def _classification_location_hint(self, strong):
        class_index = self.classifier_decoder.last_top_index
        if class_index < 0 or class_index >= DRUG_CLASS_COUNT:
            return False
        if strong:
            minimum_confidence = int(
                LOCATOR_STRONG_CONFIDENCE_MIN * 1000
            )
            minimum_margin = int(LOCATOR_STRONG_MARGIN_MIN * 1000)
        else:
            minimum_confidence = int(
                LOCATOR_HINT_CONFIDENCE_MIN * 1000
            )
            minimum_margin = int(LOCATOR_HINT_MARGIN_MIN * 1000)
        return (
            self.classifier_decoder.last_confidence_milli
            >= minimum_confidence
            and self.classifier_decoder.last_margin_milli >= minimum_margin
        )

    def _probe_candidates(
        self,
        image,
        proposals,
        output_ids,
        probe_limit,
    ):
        best_roi = None
        best_source = "无"
        best_visual_score = -1
        best_probe_score = -100000
        best_raw_length = 0
        best_candidate = NO_CANDIDATE
        best_weight = 0
        best_distance = 99
        best_class_index = UNKNOWN_CLASS_INDEX
        best_confidence_milli = 0
        best_margin_milli = 0
        best_class_accepted = False

        actual_probe_count = min(len(proposals), probe_limit)
        self.last_probe_count = actual_probe_count

        for proposal_index in range(actual_probe_count):
            proposal = proposals[proposal_index]
            roi, visual_score, source = proposal
            try:
                refined_roi = self._refine_title_roi(image, roi)
                if refined_roi is not None:
                    roi = refined_roi
                    source += "+精框"
                    visual_score += 1200
                output = self.engine.predict(image, roi)
                raw_length = self.classifier_decoder.decode(
                    output,
                    self.probe_ids,
                )
                candidate, weight, edit_distance = self.matcher.match(
                    self.probe_ids,
                    raw_length,
                )
                probe_score = self._recognition_score(
                    raw_length,
                    candidate,
                    weight,
                    visual_score,
                )
            except Exception as error:
                print("[提示] OCR 候选验证失败：", repr(error))
                continue

            # 即使 OCR 暂时只输出空白/单字，也保留视觉排序第一的窗口，
            # 让框先稳定到大标题附近，而不是退回固定引导框。
            if best_roi is None or probe_score > best_probe_score:
                best_probe_score = probe_score
                best_roi = roi
                best_source = source
                best_visual_score = visual_score
                best_raw_length = raw_length
                best_candidate = candidate
                best_weight = weight
                best_distance = edit_distance
                best_class_index = self.classifier_decoder.last_top_index
                best_confidence_milli = (
                    self.classifier_decoder.last_confidence_milli
                )
                best_margin_milli = self.classifier_decoder.last_margin_milli
                best_class_accepted = self.classifier_decoder.last_accepted
                for index in range(raw_length):
                    self.best_ids[index] = self.probe_ids[index]

        if best_roi is None:
            return None

        # 恢复最佳候选的 matcher 诊断字段，并把结果复制给主循环。
        for index in range(best_raw_length):
            output_ids[index] = self.best_ids[index]
        best_candidate, best_weight, best_distance = self.matcher.match(
            output_ids,
            best_raw_length,
        )
        self.classifier_decoder.last_top_index = best_class_index
        self.classifier_decoder.last_confidence_milli = best_confidence_milli
        self.classifier_decoder.last_margin_milli = best_margin_milli
        self.classifier_decoder.last_accepted = best_class_accepted

        self.last_source = best_source
        self.last_line_score = best_visual_score
        self.last_probe_score = best_probe_score
        self.last_raw_length = best_raw_length
        self.last_best_distance = self.matcher.last_best_distance

        return (
            best_roi,
            best_raw_length,
            best_candidate,
            best_weight,
            best_distance,
            best_probe_score,
        )

    def _result_is_usable(self, raw_length):
        best_index = self.matcher.last_best_index
        if best_index == NO_CANDIDATE or raw_length < OCR_GUIDED_MIN_RAW_LENGTH:
            return self._classification_location_hint(False)
        code_length = len(self.codes[best_index])
        distance = self.matcher.last_best_distance
        maximum_distance = max(2, code_length // 2)
        return distance <= maximum_distance

    def _update_roi(self, detected, strong):
        was_automatic = self.automatic
        if self.automatic and title_rois_compatible(self.roi, detected, 18):
            # 全宽框是“长标题首尾保护”结果，不应再与旧窄框缓慢插值，否则
            # 仍会有多帧只显示中间部分。纵向位置已经由精框确定，可立即采用。
            if detected[2] >= REFINE_MAX_ROI_HEIGHT * 6:
                self.roi = detected
            else:
                self.roi = smooth_title_roi(self.roi, detected)
            self.missed_frames = 0
            self.pending_roi = None
            self.pending_hits = 0
            return

        if (
            self.pending_roi is not None
            and title_rois_compatible(self.pending_roi, detected, 28)
        ):
            self.pending_roi = smooth_title_roi(self.pending_roi, detected)
            self.pending_hits += 1
        else:
            self.pending_roi = detected
            self.pending_hits = 1

        if strong:
            required_hits = 1
        elif was_automatic:
            # 已锁定后切换到远处新行必须更谨慎，避免品牌行和药名行来回跳。
            required_hits = VISUAL_SWITCH_HITS
        else:
            required_hits = VISUAL_LOCK_HITS
        if self.pending_hits >= required_hits:
            self.roi = self.pending_roi
            self.automatic = True
            self.missed_frames = 0
            self.pending_roi = None
            self.pending_hits = 0

    def locate_and_read(self, image, frame_number, output_ids):
        """返回 (roi, automatic, cached_result)。

        cached_result 非 None 时，主循环不需要再次执行模型推理。
        """
        if not self.available:
            return self.roi, False, None

        if self.automatic:
            should_scan = frame_number % TITLE_DETECT_INTERVAL == 0
        else:
            should_scan = (
                frame_number == 1
                or frame_number % OCR_SEARCH_INTERVAL == 0
            )
        if not should_scan:
            # 锁定后的旧代码每帧都在当前框上下重新找连通域；当药盒存在英文
            # 副标题或说明段落时，框会从正确标题逐帧漂到下方。普通帧现在只
            # 保持上一次经分类器确认的位置，精框仅在周期候选比较时执行。
            self.last_source = "保持锁定" if self.automatic else "等待搜索"
            return self.roi, self.automatic, None

        scan_start = ticks_ms()
        proposals = []

        # 主定位只使用 OCR 反馈：未锁定时纵向粗扫描；锁定后只在当前框附近
        # 做小范围位置和尺度微调。这样不会再被药盒横边、品牌图案或英文行
        # 合并成的错误连通域带偏。
        if self.automatic:
            self._add_tracking_proposals(image, proposals)
            # 旧逻辑锁定后永远只看当前行；若第一次误锁到说明小字，局部精框
            # 仍能稳定找到该行，因而不会触发丢失。每12帧重新加入全局大字行
            # 和纵向窗口，让正确标题可以凭更高分类置信度主动夺回锁定。
            global_proposals = self._generate_text_line_proposals(image)
            for proposal in global_proposals:
                self._add_proposal(
                    proposals,
                    proposal[0],
                    proposal[1],
                    proposal[2],
                )
            del global_proposals
            self._add_grid_fallbacks(image, proposals)
            probe_limit = OCR_TRACK_PROBE_LIMIT
        else:
            # 连通域行只作为低频视觉建议，不直接决定结果；最终仍由最多
            # 少量 OCR 复核。其余搜索帧只执行便宜的亮度窗口评分。
            if (
                frame_number == 1
                or frame_number % TEXT_PROPOSAL_INTERVAL == 0
            ):
                proposals = self._generate_text_line_proposals(image)
            self._add_grid_fallbacks(image, proposals)
            probe_limit = OCR_GLOBAL_PROBE_LIMIT

        if len(proposals) > GLOBAL_VISUAL_PROPOSAL_LIMIT:
            del proposals[GLOBAL_VISUAL_PROPOSAL_LIMIT:]
        self._sort_proposals_by_score(proposals)

        result = self._probe_candidates(
            image,
            proposals,
            output_ids,
            probe_limit,
        )
        del proposals
        gc.collect()

        if result is None:
            self.missed_frames += 1
            if self.missed_frames > TITLE_HOLD_FRAMES:
                self.automatic = False
                self.roi = DEFAULT_TITLE_ROI
                self.last_source = "引导"
            return self.roi, self.automatic, None

        (
            detected,
            raw_length,
            candidate,
            weight,
            edit_distance,
            probe_score,
        ) = result

        usable = self._result_is_usable(raw_length)
        if usable:
            strong = (
                (
                    candidate != NO_CANDIDATE
                    and (
                        weight >= 2
                        or self.matcher.last_best_distance
                        <= OCR_GUIDED_STRONG_DISTANCE
                    )
                )
                or self._classification_location_hint(True)
            )
            self._update_roi(detected, strong)
        else:
            # 位置锁定与文字是否通过词典门槛分离。只要局部大字精框稳定，
            # 即使 OCR 暂时识别错误，也先锁住该文字行，不再周期性退回粗框。
            if "+精框" in self.last_source:
                self._update_roi(detected, False)
                self.missed_frames = 0
            else:
                self.missed_frames += 1
                if not self.automatic:
                    if title_rois_compatible(self.roi, detected, 42):
                        self.roi = smooth_title_roi(self.roi, detected)
                    else:
                        self.roi = detected
                if self.missed_frames > TITLE_HOLD_FRAMES:
                    self.automatic = False
                    self.roi = DEFAULT_TITLE_ROI

        elapsed = ticks_diff(ticks_ms(), scan_start)
        cached_result = (
            raw_length,
            candidate,
            weight,
            edit_distance,
            elapsed,
        )
        return self.roi, self.automatic, cached_result

def draw_title_box(image, roi, automatic):
    """在推理完成后的彩色原帧上绘制绿色文字框。"""
    if not DRAW_TITLE_BOX:
        return

    x, y, width, height = roi
    image.draw_rectangle(
        (x, y, width, height),
        color=TITLE_BOX_COLOR,
        thickness=2 if automatic else 1,
    )


def setup_lcd():
    """初始化 LCD；优先兼容用户现有 lcd.init()/display() 固件。"""
    if not LCD_ENABLED:
        return None
    legacy_error = None
    try:
        import lcd

        lcd.init()
        print("[成功] LCD 已初始化：lcd.display，彩色 QVGA")
        return lcd
    except Exception as error:
        legacy_error = error

    # 官方 OpenMV 4.5 起使用 display 模块；厂商 RA8D1 固件通常仍保留 lcd。
    try:
        import display

        lcd_device = display.SPIDisplay()
        print("[成功] LCD 已初始化：display.SPIDisplay.write，彩色 QVGA")
        return lcd_device
    except Exception as error:
        print(
            "[提示] LCD 初始化失败，OCR 和 IDE 预览仍会继续：",
            repr(legacy_error),
            repr(error),
        )
        return None


def display_lcd(lcd_device, image, frame_number):
    """先把带绿色框的 RGB565 原帧送到 LCD，再允许 IDE 压缩本帧。"""
    if lcd_device is None or frame_number % LCD_INTERVAL:
        return
    try:
        try:
            lcd_device.display(image)
        except AttributeError:
            lcd_device.write(image)
    except Exception as error:
        if frame_number == 1 or frame_number % MEMORY_REPORT_INTERVAL == 0:
            print("[提示] LCD 显示失败：", repr(error))


def setup_uart():
    """按 RA8D1 厂商固件约定初始化 UART2；参数 0 实际为 115200。"""
    if not UART_ENABLED:
        return None
    try:
        from pyb import UART

        uart = UART(
            UART_PORT,
            UART_BAUDRATE,
            timeout_char=UART_TIMEOUT_CHAR,
        )
        print(
            "[成功] UART%d 已初始化：baudrate参数=%d（固件实际115200），发送UTF-8中文文本"
            % (UART_PORT, UART_BAUDRATE)
        )
        return uart
    except Exception as error:
        print("[提示] UART 初始化失败，OCR 和 LCD 仍会继续：", repr(error))
        return None


def build_uart_payloads(names):
    """启动时预编码 UART 文本，避免逐帧编码导致堆碎片。"""
    payloads = []
    for name in names:
        payloads.append((name + UART_LINE_ENDING).encode("utf-8"))
    unknown_payload = (UART_UNKNOWN_TEXT + UART_LINE_ENDING).encode("utf-8")
    return payloads, unknown_payload


def send_uart_result(
    uart,
    stable,
    frame_number,
    payloads,
    unknown_payload,
):
    """发送以 CRLF 结尾的 UTF-8 中文药名；拒识时发送“未识别到药品”。"""
    if uart is None or frame_number % UART_SEND_INTERVAL:
        return None
    if stable >= 0 and stable < len(payloads):
        payload = payloads[stable]
    else:
        payload = unknown_payload
    try:
        uart.write(payload)
        return payload
    except Exception as error:
        if frame_number == 1 or frame_number % MEMORY_REPORT_INTERVAL == 0:
            print("[提示] UART 发送失败：", repr(error))
        return None


def setup_manual_preview():
    """关闭固件后台的 RAW->JPEG 自动预览，返回手动发布所需模块。"""
    if not MANUAL_IDE_PREVIEW:
        return None
    try:
        import omv

        omv.disable_fb(True)
        print(
            "IDE 预览：手动 JPEG，每%d帧，质量%d"
            % (PREVIEW_INTERVAL, PREVIEW_QUALITY)
        )
        return omv
    except Exception as error:
        print("[提示] 无法启用手动 IDE 预览：", repr(error))
        return None


def publish_preview(image, frame_number, omv_module):
    """循环末尾原地压缩并发布；调用后不得再读取本帧 RGB565 像素。"""
    if omv_module is None or frame_number % PREVIEW_INTERVAL:
        return
    try:
        image.compress(quality=PREVIEW_QUALITY)
        preview_size = image.size()
        if preview_size > PREVIEW_BUFFER_LIMIT:
            print(
                "[提示] IDE JPEG 过大：%d/%d bytes，本帧不发布"
                % (preview_size, PREVIEW_BUFFER_LIMIT)
            )
            return
        if frame_number % FRAME_INTEGRITY_INTERVAL == 0:
            print(
                "[预览] JPEG：%d/%d bytes"
                % (preview_size, PREVIEW_BUFFER_LIMIT)
            )
        omv_module.disable_fb(False)
        image.flush()
    except Exception as error:
        print("[提示] 手动 IDE 预览失败：", repr(error))
    finally:
        omv_module.disable_fb(True)


def memory_free():
    gc.collect()
    try:
        return gc.mem_free()
    except AttributeError:
        return -1


def print_memory(stage):
    free_bytes = memory_free()
    if free_bytes >= 0:
        print("%s%d bytes" % (stage, free_bytes))


def print_frame_integrity(stage, image, sensor):
    """比较推理前后原帧统计，区分采集、模型和 IDE 预览故障。"""
    try:
        mean, stdev, minimum, maximum = luminance_statistics(image)
        print(
            "[帧校验-%s] 均值/标准差=%d/%d"
            % (
                stage,
                mean,
                stdev,
            )
        )
    except Exception as error:
        print("[提示] 无法执行帧完整性检查：", repr(error))


class MlEngine:
    name = "ml.Model"

    def __init__(self, model_path):
        import ml

        try:
            self.model = ml.Model(
                model_path,
                load_to_fb=MODEL_LOAD_TO_FB,
            )
        except TypeError:
            # 新版统一分配器已移除该参数，会自动选择合适的内存池。
            self.model = ml.Model(model_path)
        self.normalization_class = None
        try:
            from ml.preprocessing import Normalization

            self.normalization_class = Normalization
        except Exception:
            pass
        print("[成功] ml.Model 已加载")
        print("输入形状：", self.model.input_shape)
        print("输入类型：", self.model.input_dtype)
        print("输出形状：", self.model.output_shape)
        print("输出类型：", self.model.output_dtype)
        try:
            print("Tensor arena：", self.model.ram, "bytes")
        except AttributeError:
            pass

    def predict(self, image, roi=None):
        if roi is not None:
            roi = sanitize_roi(roi)
            if roi is None:
                raise ValueError("无效 OCR ROI")
        model_input = image
        temporary_image = None
        if roi is not None:
            if self.normalization_class is not None:
                model_input = self.normalization_class(roi=roi)(image)
            else:
                temporary_image = image.copy(
                    roi=roi,
                    x_scale=IMAGE_WIDTH / roi[2],
                    y_scale=IMAGE_HEIGHT / roi[3],
                )
                model_input = temporary_image
        outputs = self.model.predict([model_input])
        if temporary_image is not None:
            del temporary_image
        if len(outputs) != 1:
            raise ValueError("模型输出张量数量错误：%d" % len(outputs))
        output = outputs[0]
        try:
            return output.flat
        except AttributeError:
            return output


class TfEngine:
    name = "tf.classify"

    def __init__(self, model_path):
        import tf

        self.tf = tf
        self.model = tf.load(
            model_path,
            load_to_fb=MODEL_LOAD_TO_FB,
        )
        self.ocr_unsharp_available = OCR_UNSHARP
        print("[成功] tf.load 已加载到帧缓冲区")
        print("模型对象：", self.model)

    def predict(self, image, roi=None):
        if roi is not None:
            roi = sanitize_roi(roi)
            if roi is None:
                raise ValueError("无效 OCR ROI")
        if roi is None:
            raise ValueError("旧tf后端必须提供明确的OCR ROI")
        # 旧版 tf.classify 的标准接口是“原始帧 + roi=”。后端由该 ROI 直接
        # 生成模型要求的 192x32 UINT8 张量。先 image.copy(roi) 再把可变尺寸
        # 小图作为整图传入，会进入滑窗缩放路径，真机上会造成输入比例/采样偏差。
        # PRE/POST 帧统计已证明直接 ROI 推理不会破坏用于 IDE 显示的原始帧。
        results = self.tf.classify(
            self.model,
            image,
            roi=roi,
            min_scale=1.0,
            scale_mul=0.5,
            x_overlap=0.0,
            y_overlap=0.0,
        )
        if not results:
            raise RuntimeError("tf.classify 没有返回推理窗口")
        if len(results) != 1:
            print("[警告] 推理窗口数量：", len(results), "，仅使用第一个")
        output = results[0].output()
        return output


def load_engine(model_path):
    try:
        import ml  # noqa: F401
    except ImportError:
        print("当前固件没有 ml 模块，回退到旧版 tf 模块。")
        return TfEngine(model_path)
    return MlEngine(model_path)


def try_auto_focus(sensor):
    if not AUTO_FOCUS:
        return
    try:
        trigger = sensor.IOCTL_TRIGGER_AUTO_FOCUS
    except AttributeError:
        print("[提示] 当前固件没有 OV5640 自动对焦常量")
        return
    try:
        print("正在触发 OV5640 自动对焦，请保持药名位于画面中央……")
        sensor.ioctl(trigger)
        try:
            sensor.ioctl(sensor.IOCTL_WAIT_ON_AUTO_FOCUS, 4000)
        except AttributeError:
            try:
                time.sleep_ms(2500)
            except AttributeError:
                time.sleep(2.5)
        print("[成功] 自动对焦流程完成")
    except Exception as error:
        print("[提示] 相机可能是固定焦距版本：", repr(error))


def read_title_statistics(sensor):
    image = sensor.snapshot()
    mean, stdev, minimum, maximum = luminance_statistics(image, METER_ROI)
    try:
        histogram = luminance_histogram(image, METER_ROI, 64)
        p20 = luminance_histogram_value(histogram.get_percentile(0.20))
        p75 = luminance_histogram_value(histogram.get_percentile(0.75))
        p95 = luminance_histogram_value(histogram.get_percentile(0.95))
        del histogram
    except Exception:
        p20 = clamp(mean - stdev, 0, 255)
        p75 = clamp(mean + stdev // 2, 0, 255)
        p95 = clamp(mean + (stdev * 3) // 2, 0, 255)
    return mean, stdev, p20, p75, p95


def apply_manual_camera(sensor, exposure_us, gain_db, settle_frames=2, report=False):
    """写入手动曝光/增益，并尽量读回传感器实际值。"""
    requested_exposure = int(exposure_us)
    requested_gain = float(gain_db)
    try:
        # 该 RA8D1 固件分支用位置参数兼容性更稳定。
        sensor.set_auto_exposure(False, requested_exposure)
    except TypeError:
        sensor.set_auto_exposure(False, exposure_us=requested_exposure)
    try:
        sensor.set_auto_gain(False, requested_gain)
    except TypeError:
        sensor.set_auto_gain(False, gain_db=requested_gain)
    if settle_frames > 0:
        sensor.skip_frames(n=settle_frames)

    actual_exposure = requested_exposure
    actual_gain = requested_gain
    try:
        actual_exposure = int(sensor.get_exposure_us())
    except Exception:
        pass
    try:
        actual_gain = float(sensor.get_gain_db())
    except Exception:
        pass
    if report:
        print(
            "曝光请求/实测：%d/%d us；增益请求/实测：%.1f/%.1f dB"
            % (
                requested_exposure,
                actual_exposure,
                requested_gain,
                actual_gain,
            )
        )
    return actual_exposure, actual_gain


def settle_and_lock_exposure(sensor):
    exposure_us = EXPOSURE_INITIAL_US
    gain_db = GAIN_INITIAL_DB
    try:
        apply_manual_camera(sensor, exposure_us, gain_db, settle_frames=5)
    except Exception as error:
        print("[提示] 无法设置初始手动曝光/增益：", repr(error))
    try:
        sensor.set_auto_whitebal(False)
    except Exception:
        pass

    title_mean = 0
    title_stdev = 0
    p20 = 0
    p75 = 0
    p95 = 0
    consecutive_hits = 0
    for round_index in range(EXPOSURE_ADJUST_ROUNDS):
        try:
            title_mean, title_stdev, p20, p75, p95 = read_title_statistics(
                sensor
            )
        except Exception as error:
            print("[提示] 无法读取标题区亮度：", repr(error))
            break

        # P95 能看到占画面较小的白药盒；P75-P20 在大面积黑背景下会误判。
        tonal_range = p95 - p20
        if tonal_range < METER_MIN_RANGE:
            print(
                "[提示] 第%d轮计量区层次偏低，继续尝试增亮" % (round_index + 1)
            )

        acceptable = (
            METER_ACCEPT_LOW <= p75 <= METER_ACCEPT_HIGH
            and p95 < METER_HIGHLIGHT_LIMIT
        )
        if acceptable:
            consecutive_hits += 1
            if consecutive_hits >= 2:
                break
            sensor.skip_frames(n=2)
            continue
        consecutive_hits = 0

        new_exposure = exposure_us
        new_gain = gain_db
        # 计量区明显欠曝时优先增亮；少量反光点不能让整幅画面继续变暗。
        if p75 < METER_ACCEPT_LOW:
            if exposure_us < EXPOSURE_MAX_US:
                new_exposure = min(
                    EXPOSURE_MAX_US,
                    int(float(exposure_us) * 1.25),
                )
            elif gain_db < GAIN_MAX_DB:
                new_gain = min(GAIN_MAX_DB, gain_db + 1.0)
        elif p95 >= METER_HIGHLIGHT_LIMIT or p75 > METER_ACCEPT_HIGH:
            if gain_db > GAIN_MIN_DB:
                new_gain = max(GAIN_MIN_DB, gain_db - 1.0)
            else:
                new_exposure = max(
                    EXPOSURE_MIN_US,
                    int(float(exposure_us) * 0.80),
                )
        if new_exposure == exposure_us and new_gain == gain_db:
            break
        try:
            apply_manual_camera(
                sensor,
                new_exposure,
                new_gain,
                settle_frames=4,
            )
            exposure_us = new_exposure
            gain_db = new_gain
        except Exception as error:
            print("[提示] 标题区曝光补偿失败：", repr(error))
            break

    try:
        title_mean, title_stdev, p20, p75, p95 = read_title_statistics(sensor)
        print(
            "搜索区亮度：均值 %d；P20/P75/P95=%d/%d/%d；标准差 %d"
            % (title_mean, p20, p75, p95, title_stdev)
        )
        if p75 < METER_ACCEPT_LOW:
            print("[警告] 药盒浅色区域仍偏暗，请增加漫射光")
        elif p95 >= METER_HIGHLIGHT_LIMIT:
            print("[警告] 药盒存在过曝高光，请减少反光")
    except Exception as error:
        print("[提示] 无法复核标题区亮度：", repr(error))
    try:
        apply_manual_camera(
            sensor,
            exposure_us,
            gain_db,
            settle_frames=4,
            report=True,
        )
    except Exception as error:
        print("[提示] 无法复核手动曝光/增益：", repr(error))
    return exposure_us, gain_db


def setup_sensor(preview_module=None):
    import sensor

    sensor.reset()
    # sensor.reset() 之后立即关掉自动发布，连曝光/对焦阶段也不走故障路径。
    if preview_module is not None:
        preview_module.disable_fb(True)
    # 单张 RGB565 原帧同时用于 LAB 亮度定位、灰度模型 ROI 推理和 LCD 彩显。
    # 不创建第二张 QVGA 灰度副本，避免 294 KB 模型加载后内存不足。
    sensor.set_pixformat(sensor.RGB565 if COLOR_CAPTURE else sensor.GRAYSCALE)
    # 始终保持 320x240 帧和算法坐标；适度缩放只改变 OV5640 的传感器读出范围。
    sensor.set_framesize(sensor.QVGA)
    if CAMERA_READOUT_SIZE is not None:
        try:
            sensor.ioctl(
                sensor.IOCTL_SET_READOUT_WINDOW,
                CAMERA_READOUT_SIZE,
            )
            print(
                "相机采样：OV5640约1.5倍居中读出 -> 320x240"
            )
            try:
                actual_readout = sensor.ioctl(
                    sensor.IOCTL_GET_READOUT_WINDOW
                )
                print("传感器实际读出窗口：", actual_readout)
            except Exception:
                pass
        except Exception as error:
            print(
                "[提示] 固件不支持适度读出缩放，保持QVGA全视野：",
                repr(error),
            )
    else:
        print("相机采样：原生QVGA 320x240（全视野，无中央放大）")
    try:
        # 保持自然成像，不额外拉高传感器对比度。
        sensor.set_contrast(CAMERA_CONTRAST)
        sensor.set_brightness(0)
    except Exception:
        pass
    if AUTO_FOCUS:
        print("请把药名放到画面中央并保持不动，正在稳定相机和自动对焦……")
    else:
        print("手动焦点模式：不会触发OV5640自动对焦，请把药名放到画面中央。")
    if CUSTOM_EXPOSURE_CONTROL:
        exposure_us, gain_db = settle_and_lock_exposure(sensor)
        try_auto_focus(sensor)
        sensor.skip_frames(time=600)
        try_auto_focus(sensor)
        try:
            apply_manual_camera(
                sensor,
                exposure_us,
                gain_db,
                settle_frames=4,
                report=True,
            )
        except Exception as error:
            print("[提示] 对焦后无法重新锁定曝光：", repr(error))
    else:
        print("相机：使用 OV5640 自动曝光/增益，不执行程序测光")
        try:
            sensor.set_auto_exposure(True)
            sensor.set_auto_gain(True)
            if COLOR_CAPTURE:
                sensor.set_auto_whitebal(True)
        except Exception as error:
            print("[提示] 无法显式启用自动曝光/增益：", repr(error))
        sensor.skip_frames(time=1200)
        # AUTO_FOCUS=False 时两次调用都会立即返回，保留手动调好的焦点。
        try_auto_focus(sensor)
        sensor.skip_frames(time=600)
        try_auto_focus(sensor)
    sensor.skip_frames(time=500)
    try:
        mean, stdev, minimum, maximum = luminance_statistics(sensor.snapshot())
        print("全景亮度：均值", mean, "；标准差", stdev)
        print("完整 RGB565 画面用于标题定位、彩色显示和绿色画框")
    except Exception as error:
        print("[提示] 无法读取图像统计：", repr(error))
    return sensor


def ticks_ms():
    try:
        return time.ticks_ms()
    except AttributeError:
        return int(time.time() * 1000)


def ticks_diff(end, start):
    try:
        return time.ticks_diff(end, start)
    except AttributeError:
        return end - start


def main():
    print("=== 中文药名直接分类 V6.3.4 彩色 LCD + UTF-8 UART 启动 ===")
    print("程序版本：", BUILD_ID)
    model_path = first_existing_path(MODEL_NAME)
    charset_path = first_existing_path(CHARSET_NAME)
    dictionary_path = first_existing_path(DRUG_NAMES_NAME)
    if model_path is None:
        raise OSError("未找到模型文件：" + MODEL_NAME)
    if charset_path is None:
        raise OSError("未找到字符表：" + CHARSET_NAME)
    if dictionary_path is None:
        raise OSError("未找到药名词典：" + DRUG_NAMES_NAME)

    # 早先两代 UINT8 模型大小恰好相同，曾经不能只用文件大小判断版本。
    # 新闭集分类模型仍在加载前按内容校验，避免旧权重或旧词典造成类别错位。
    validate_deployment_files(model_path, charset_path, dictionary_path)
    gc.collect()

    try:
        print("模型大小：", os.stat(model_path)[6], "bytes")
    except Exception:
        pass

    # load_to_fb=True 使用帧缓冲栈，必须先完成相机帧格式和尺寸初始化；
    # 模型加载后不再改变帧缓冲布局，只在其上方使用可释放的临时工作区。
    preview_module = setup_manual_preview()
    sensor = setup_sensor(preview_module)
    gc.collect()
    print_memory("加载模型前可用内存：")
    engine = load_engine(model_path)
    print("推理后端：", engine.name)
    print_memory("加载模型后可用内存：")
    # 先保证 294 KB 模型成功进入帧缓冲栈，再初始化 LCD 的小型驱动缓冲区。
    lcd_device = setup_lcd()
    uart = setup_uart()

    # 大模型先进入帧缓冲区，再创建大量小字符串/列表，避免旧 MicroPython
    # 堆被字符表和词典切碎后无法申请连续模型块。
    charset = load_charset(charset_path)
    names, codes, max_name_length = load_drug_dictionary(
        dictionary_path,
        charset,
    )
    validate_deployment_assets(charset, names)
    uart_payloads, uart_unknown_payload = build_uart_payloads(names)
    print("字符数量：", len(charset))
    print("目标药名：", len(names))
    print("最长药名：", max_name_length)

    matcher = DrugMatcher(codes, max_name_length)
    classifier_decoder = DrugClassifierDecoder(codes)
    voter = WeightedVoter(len(names))
    raw_ids = [0] * TIME_STEPS
    # 普通模式只需要类别 ID；诊断模式保留字符表以显示真机原始结果。
    if not PRINT_RAW_STATUS:
        del charset
    gc.collect()

    title_locator = TitleLocator(
        engine,
        matcher,
        codes,
        classifier_decoder,
    )
    print("[V6.3.4] RGB565原帧+ROI直送；34药名+未知类直接分类")
    print("请让药名保持横向；全局搜索最多复核4个窗口，锁定后每帧只推理1次。")
    print("完整画面：%d×%d RGB565 彩色" % (FRAME_WIDTH, FRAME_HEIGHT))
    print("分类输入：%d×%d；请让横向大标题位于画面中上部框内。" % (IMAGE_WIDTH, IMAGE_HEIGHT))
    print("LCD：每帧显示彩色原图；绿色框在推理后绘制，不会进入模型输入。")
    print("UART2：每帧发送UTF-8中文药名+CRLF；未稳定发送“未识别到药品”；参数0=115200。")
    print("精框不增加模型次数；粗窗口只做快速亮度统计，不再逐一执行模型。")

    frame_number = 0
    fps_window_start = ticks_ms()
    fps_window_frames = 0
    while True:
        frame_number += 1
        image = sensor.snapshot()
        integrity_frame = frame_number % FRAME_INTEGRITY_INTERVAL == 0
        if integrity_frame:
            print_frame_integrity("PRE", image, sensor)
        start = ticks_ms()
        title_roi, automatic_roi, cached_result = title_locator.locate_and_read(
            image,
            frame_number,
            raw_ids,
        )
        if cached_result is None:
            output = engine.predict(image, title_roi)
            raw_length = classifier_decoder.decode(output, raw_ids)
            candidate, weight, edit_distance = matcher.match(
                raw_ids,
                raw_length,
            )
        else:
            (
                raw_length,
                candidate,
                weight,
                edit_distance,
                scan_elapsed,
            ) = cached_result
        if integrity_frame:
            print_frame_integrity("POST", image, sensor)
        stable, changed, score, margin = voter.update(candidate, weight)
        send_uart_result(
            uart,
            stable,
            frame_number,
            uart_payloads,
            uart_unknown_payload,
        )
        elapsed = ticks_diff(ticks_ms(), start)
        # 先完成 ROI 推理，再在完整帧上画框，避免边框进入模型输入。
        draw_title_box(image, title_roi, automatic_roi)
        if integrity_frame:
            print_frame_integrity("DRAW", image, sensor)

        if PRINT_RAW_IDS:
            print("原始类别：", raw_ids[:raw_length])
        if PRINT_CANDIDATE and candidate != NO_CANDIDATE:
            print(
                "候选：%s | 距离：%d | 权重：%d"
                % (names[candidate], edit_distance, weight)
            )
        if frame_number % STATUS_INTERVAL == 0:
            roi_mode = "锁定" if automatic_roi else "搜索"
            print(
                "[%d] ROI：%s (%d,%d,%d,%d) | 来源：%s | 精框：%d区/阈值%d | 候选：%d | 位置分：%d | OCR分：%d | 距离：%d"
                % (
                    frame_number,
                    roi_mode,
                    title_roi[0],
                    title_roi[1],
                    title_roi[2],
                    title_roi[3],
                    title_locator.last_source,
                    title_locator.last_refine_components,
                    title_locator.last_refine_dark_limit,
                    title_locator.last_probe_count,
                    title_locator.last_line_score,
                    title_locator.last_probe_score,
                    title_locator.last_best_distance,
                )
            )
            class_index = classifier_decoder.last_top_index
            if class_index >= 0 and class_index < len(names):
                class_name = names[class_index]
            else:
                class_name = "<未知>"
            confidence_milli = classifier_decoder.last_confidence_milli
            margin_milli = classifier_decoder.last_margin_milli
            print(
                "[%d] 分类：%s | 置信：%d.%d%% | 差值：%d.%d%% | %s"
                % (
                    frame_number,
                    class_name,
                    confidence_milli // 10,
                    confidence_milli % 10,
                    margin_milli // 10,
                    margin_milli % 10,
                    "接受" if classifier_decoder.last_accepted else "拒绝",
                )
            )
        if changed:
            if stable == NO_CANDIDATE:
                print("[%d] OCR：<等待稳定>" % frame_number)
            else:
                print(
                    "[%d] OCR：%s | 推理：%d ms | 分数：%d"
                    % (frame_number, names[stable], elapsed, score)
                )
        elif frame_number % STATUS_INTERVAL == 0:
            if stable == NO_CANDIDATE:
                if PRINT_RAW_STATUS:
                    raw_text = ids_to_text(raw_ids, raw_length, charset)
                    nearest_index = matcher.last_best_index
                    if nearest_index == NO_CANDIDATE:
                        print(
                            "[%d] 原始：%s | 最近：<无> | 长度：%d | %d ms"
                            % (frame_number, raw_text, raw_length, elapsed)
                        )
                    else:
                        if matcher.last_prefix_match:
                            print(
                                "[%d] 原始：%s | 前缀候选：%s | %d ms"
                                % (
                                    frame_number,
                                    raw_text,
                                    names[nearest_index],
                                    elapsed,
                                )
                            )
                        else:
                            print(
                                "[%d] 原始：%s | 最近：%s | 距离：%d/%d | %d ms"
                                % (
                                    frame_number,
                                    raw_text,
                                    names[nearest_index],
                                    matcher.last_best_distance,
                                    matcher.last_second_distance,
                                    elapsed,
                                )
                            )
                else:
                    print(
                        "[%d] OCR：<等待稳定> | 推理：%d ms"
                        % (frame_number, elapsed)
                    )
            else:
                print(
                    "[%d] OCR：%s | 推理：%d ms | 领先：%d"
                    % (frame_number, names[stable], elapsed, margin)
                )

        if frame_number % MEMORY_REPORT_INTERVAL == 0:
            print_memory("周期可用内存：")
        # LCD 必须在 JPEG 原地压缩前显示未压缩的 RGB565 彩色帧。
        display_lcd(lcd_device, image, frame_number)
        # 必须是本帧最后一次图像操作：下一次 snapshot() 会重新采集 RGB565 帧。
        publish_preview(image, frame_number, preview_module)
        fps_window_frames += 1
        if fps_window_frames >= FPS_REPORT_INTERVAL:
            fps_now = ticks_ms()
            fps_elapsed = ticks_diff(fps_now, fps_window_start)
            if fps_elapsed > 0:
                fps_times_ten = fps_window_frames * 10000 // fps_elapsed
                print(
                    "[%d] OCR循环：%d.%d FPS | LCD：每%d帧 | IDE预览：每%d帧"
                    % (
                        frame_number,
                        fps_times_ten // 10,
                        fps_times_ten % 10,
                        LCD_INTERVAL,
                        PREVIEW_INTERVAL,
                    )
                )
            fps_window_start = fps_now
            fps_window_frames = 0
        if RUN_ONCE:
            break


if __name__ == "__main__":
    try:
        main()
    finally:
        # Ctrl+C 或异常退出后恢复 IDE 默认帧缓冲行为，便于继续调试。
        try:
            import omv

            omv.disable_fb(False)
        except Exception:
            pass
