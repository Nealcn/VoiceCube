"""语音速记协调器 — 状态机"""
import asyncio
import logging
from typing import Optional

from .protocol import StateEvent, AudioFrame, ui_state_payload
from .ble import BleClient
from .asr_client import AsrClient
from .input_injector import paste_text
from .llm_translation_client import LLMTranslationClient

logger = logging.getLogger(__name__)


class Coordinator:
    """BLE + ASR + 输入注入协调器"""

    def __init__(self, ble: BleClient, asr_client: AsrClient):
        self._ble = ble
        self._asr = asr_client
        self._session_id: Optional[int] = None
        self._recording = False
        self._audio_queue: asyncio.Queue = None  # 流式音频队列

        # UI 回调
        self.on_status: Optional[callable] = None
        self.on_partial_text: Optional[callable] = None
        self.on_final_text: Optional[callable] = None
        self.on_device_connected: Optional[callable] = None
        self.on_device_disconnected: Optional[callable] = None

        # LLM 翻译 + 润色
        self._translator = LLMTranslationClient()
        self._translation_enabled = False
        self._polish_enabled = False
        self._polish_prompt = ""
        self._polish_before_translate = True

        # 配置
        self.paste_on_final = True
        self.press_enter_after_paste = False
        self.asr_server_url = "ws://localhost:8080"

        # 注册 BLE 回调
        self._ble.on_audio_frame = self._on_audio_frame
        self._ble.on_state_event = self._on_state_event
        self._ble.on_connected = self._on_ble_connected
        self._ble.on_disconnected = self._on_ble_disconnected

        # 注册 ASR 回调
        self._asr.on_partial = self._on_asr_partial
        self._asr.on_final = lambda t: asyncio.create_task(self._on_asr_final(t))
        self._asr.on_error = self._on_asr_error

    async def start(self):
        """启动协调器"""
        self._set_status("就绪")
        # ASR 连接在后台进行，不阻塞
        asyncio.create_task(self._connect_asr())

    async def _connect_asr(self):
        """后台连接 ASR"""
        ok = await self._asr.start()
        if not ok:
            self._set_status("ASR 未连接")

    def configure_translation(self, enabled: bool, api_key: str, base_url: str,
                               model: str, target_language: str):
        """配置 LLM 翻译"""
        self._translation_enabled = enabled
        self._translator.update_config(api_key, base_url, model, target_language)

    def configure_polish(self, enabled: bool, prompt: str, before_translate: bool):
        """配置 LLM 润色"""
        self._polish_enabled = enabled
        self._polish_prompt = prompt
        self._polish_before_translate = before_translate

    async def shutdown(self):
        await self._asr.stop()
        await self._translator.close()
        await self._ble.disconnect()

    def _set_status(self, status: str):
        logger.info("状态: %s", status)
        if self.on_status:
            self.on_status(status)

    # ---- BLE 回调 ----

    def _on_ble_connected(self):
        self._set_status("已连接")
        if self.on_device_connected:
            self.on_device_connected(self._ble.device_name)

    def _on_ble_disconnected(self):
        self._recording = False
        self._set_status("已断开")
        if self.on_device_disconnected:
            self.on_device_disconnected()

    def _on_state_event(self, event: StateEvent):
        logger.debug("状态事件: %s", event)
        if event.event == "button_down":
            self._handle_button_down(event)
        elif event.event == "button_up":
            self._handle_button_up(event)

    def _handle_button_down(self, event: StateEvent):
        """按下：开始录音 + 启动 ASR 流式会话"""
        if event.button == "primary" and not self._recording:
            self._recording = True
            self._session_id = event.session_id
            self._set_status("录音中…")
            asyncio.create_task(self._stream_start())

    async def _stream_start(self):
        """启动 ASR 会话（流式模式下在录音开始时即创建）"""
        self._audio_queue = asyncio.Queue()
        ok = await self._asr.start_session()
        if not ok:
            self._set_status("ASR 会话失败")
            self._recording = False
            self._audio_queue = None
            return
        # 启动队列消费者（顺序发送，不会乱序）
        asyncio.create_task(self._stream_send_loop())

    def _handle_button_up(self, event: StateEvent):
        # button_up 仅用于更新 session_id
        if event.session_id is not None:
            self._session_id = event.session_id

    def _on_audio_frame(self, frame: AudioFrame):
        # Accept frame if session matches, or if no session set yet
        if self._session_id is not None and frame.session_id != self._session_id:
            return
        if self._session_id is None:
            self._session_id = frame.session_id
            logger.info("从音频帧设置 session_id=%d", self._session_id)

        if frame.is_end():
            self._recording = False
            logger.info("收到结束帧，发送 is_last 并等待 ASR 最终结果")
            # 发送最后一帧（is_last=True）触发服务端结束识别
            asyncio.create_task(self._stream_finish())
        else:
            # 入队等待顺序发送（不直接 create_task，避免乱序）
            if self._audio_queue is not None:
                self._audio_queue.put_nowait(frame.payload)

    async def _stream_send_loop(self):
        """队列消费者：按序逐帧发送音频到 ASR，直到收到 None 哨兵"""
        q = self._audio_queue
        if q is None:
            return
        while True:
            chunk = await q.get()
            if chunk is None:
                break  # 哨兵：队列结束
            await self._asr.send_audio(chunk, is_last=False)

    async def _stream_finish(self):
        """发送结束帧 + 等待 ASR 最终结果"""
        # 发哨兵等队列消费者结束
        if self._audio_queue is not None:
            self._audio_queue.put_nowait(None)
            self._audio_queue = None
        await self._asr.send_audio(b"", is_last=True)
        await self._asr.stop_session()

    # _process_audio 已移除 — 改用流式发送 (_stream_start / _stream_finish)

    # ---- ASR 回调 ----

    def _on_asr_partial(self, text: str):
        if self.on_partial_text:
            self.on_partial_text(text)

    async def _on_asr_final(self, text: str):
        """最终 ASR 结果（协程）"""
        # 跳过无实际文本的结果（录音过短或噪音）
        if text.startswith("{") and '"text"' not in text:
            logger.debug("ASR 返回空文本(录音过短), 跳过")
            return

        logger.info("ASR 最终结果: %s", text)
        self._set_status("就绪")

        if self.on_final_text:
            self.on_final_text(text)

        if not text.strip():
            return

        # 直接粘贴原文（润色/翻译/保存仅在点击按钮时执行）
        content = text

        # 通知固件：有结果了（pending confirmation）
        await self._ble.send_ui_state("pending_confirmation")

        # 自动粘贴
        if self.paste_on_final:
            self._set_status("粘贴中…")
            ok = await asyncio.to_thread(paste_text, content, self.press_enter_after_paste)
            if ok:
                self._set_status("已粘贴")
            else:
                self._set_status("粘贴失败")
            await asyncio.sleep(1)
            self._set_status("就绪")

        # 通知固件：返回就绪（蓝色）
        await self._ble.send_ui_state("ready")

    def _on_asr_error(self, message: str):
        logger.error("ASR 错误: %s", message)
        self._set_status(f"错误: {message}")
