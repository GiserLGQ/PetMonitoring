// ===== ESP32-CAM 控制台脚本 =====
// 接口约定(与固件 app_httpd.cpp 对应):
//   GET /status           -> JSON 状态
//   GET /control?var=X&val=Y  -> 设置参数(framesize/quality/wb_mode/led_intensity)
//   GET /capture          -> 单张 JPEG
//   GET /stream           -> MJPEG 视频流(内网:81 直连 / 隧道: 由 cloudflared 按路径转发)
//   GET /ptz?dir=up|down|left|right|talk&action=down|up -> 云台遥控/对话
//   GET /rec?state=start|stop -> 录制到 NAS

const $ = (id) => document.getElementById(id);

console.log('[ESP32-CAM] script.js v10 已加载'); // 用于确认浏览器没有使用缓存的旧脚本

// 视频流在独立的 81 端口(esp-idf httpd 每端口单线程, 流会占死所在端口,
// 因此控制接口用 80 端口、视频流专用 81 端口, 与官方示例一致)
// - 内网直连: 直接用 :81 绝对地址
// - 隧道/公网: Cloudflare 不代理 81 端口, 用相对路径 /stream,
//   由 cloudflared 按 path 规则(path: ^/stream)转发到内网 192.168.0.32:81
const IS_LAN = /^(192\.168|10\.|172\.(1[6-9]|2\d|3[01])\.)/.test(location.hostname)
            || location.hostname === 'localhost';

const STREAM_URL = IS_LAN
    ? location.protocol + '//' + location.hostname + ':81/stream'
    : location.origin + '/stream';

// ---------- 快照降级模式 ----------
// 流不可用时(隧道未配 /stream 规则、连接被中间设备掐断等),
// 自动改为每 350ms 拉一帧 /capture 单图, 画面变慢(约3FPS)但不至于黑屏
let snapTimer = null;

function stopSnap() {
    if (snapTimer) { clearInterval(snapTimer); snapTimer = null; }
    const img = $('stream');
    if (img.dataset.snapUrl) {
        URL.revokeObjectURL(img.dataset.snapUrl);
        delete img.dataset.snapUrl;
    }
}

function startSnap() {
    if (snapTimer) return;
    console.warn('[ESP32-CAM] 视频流不可用, 降级为快照模式(约3FPS)');
    const pull = () => fetch('/capture')
        .then(r => { if (!r.ok) throw new Error(r.status); return r.blob(); })
        .then(b => {
            if (!streaming) return;
            const img = $('stream');
            if (img.dataset.snapUrl) URL.revokeObjectURL(img.dataset.snapUrl);
            img.dataset.snapUrl = URL.createObjectURL(b);
            img.src = img.dataset.snapUrl;
            $('viewer-hint').textContent = '快照模式(视频流降级)';
        })
        .catch(() => {});   // 静默重试
    pull();
    snapTimer = setInterval(pull, 350);
}

// ---------- 通用: 发送控制命令 ----------
function send(varName, val) {
    fetch('/control?var=' + encodeURIComponent(varName) + '&val=' + encodeURIComponent(val))
        .then(r => r.text())
        .then(t => { if (t.trim() !== '' && t.trim() !== '1') console.warn('设置失败:', varName, '=', val, '返回:', t); })
        .catch(err => console.warn('网络错误:', err));
}

// ---------- 视频流 启动/停止 ----------
let streaming = true;

// ---------- 长时间未操作自动关闭视频 ----------
// 视频流全局同一时刻只能有 1 个观众(esp32 httpd 单线程), 长期挂机会占着席位。
// 规则: 连续 10 分钟无任何鼠标/触摸/键盘操作则自动关闭视频;
// 有操作即重置计时; 切到后台标签页照常计时(后台同样占用席位)。
const STREAM_IDLE_MS = 10 * 60 * 1000;   // 想改时长改这里
let idleTimer = null;

function stopIdleTimer() {
    if (idleTimer) { clearTimeout(idleTimer); idleTimer = null; }
}

function resetIdleTimer() {
    stopIdleTimer();
    if (!streaming) return;
    idleTimer = setTimeout(() => {
        streaming = false;
        applyStream();
        $('viewer-hint').textContent = '长时间未操作, 视频已自动关闭';
        console.log('[ESP32-CAM] 长时间未操作, 自动关闭视频释放观看席位');
    }, STREAM_IDLE_MS);
}

function applyStream() {
    const img = $('stream');
    const hint = $('viewer-hint');
    if (streaming) {
        stopSnap();
        img.classList.remove('off');
        hint.classList.add('placeholder');
        hint.textContent = '视频加载中, 请稍候…';
        img.src = STREAM_URL;
        resetIdleTimer();
    } else {
        stopSnap();
        stopIdleTimer();
        img.src = '';
        img.classList.add('off');
        hint.classList.remove('placeholder');
        hint.textContent = '视频已停止';
    }
    $('toggle-stream').textContent = streaming ? '停止视频' : '启动视频';
}

$('toggle-stream').onclick = () => {
    streaming = !streaming;
    applyStream();
};

// 流意外断开(如路由器重启、隧道断流)时: 保持播放状态, 降级为快照轮询
$('stream').onerror = () => {
    if (streaming) startSnap();
};
// 视频成功加载后: 停掉快照模式, 清除提示文字(实时流优先)
$('stream').onload = () => {
    if (streaming) { stopSnap(); $('viewer-hint').textContent = ''; }
};

// ---------- 截图 ----------
$('get-still').onclick = () => window.open('/capture');

// ---------- 分辨率 ----------
$('framesize').onchange = (e) => send('framesize', e.target.value);

// ---------- 画质 ----------
$('quality').oninput = (e) => { $('quality-val').textContent = e.target.value; };
$('quality').onchange = (e) => send('quality', e.target.value);

// ---------- 白平衡模式 ----------
$('wb_mode').onchange = (e) => send('wb_mode', e.target.value);

// ---------- LED 补光灯 ----------
let ledOn = false;

$('led_intensity').oninput = (e) => {
    $('led-val').textContent = e.target.value;
};
$('led_intensity').onchange = (e) => {
    ledOn = Number(e.target.value) > 0;
    $('led-toggle').textContent = ledOn ? '关' : '开';
    send('led_intensity', e.target.value);
};
$('led-toggle').onclick = () => {
    ledOn = !ledOn;
    const duty = ledOn ? 255 : 0;
    $('led_intensity').value = duty;
    $('led-val').textContent = duty;
    $('led-toggle').textContent = ledOn ? '关' : '开';
    send('led_intensity', duty);
};

// ---------- 云台遥控 ----------
// 方向键: 按住发 action=down, 松开发 action=up (后期固件里驱动舵机)
// 对话键: 长按开始(action=down), 松开结束(action=up)
function ptzSend(dir, action) {
    fetch('/ptz?dir=' + dir + '&action=' + action)
        .catch(err => console.warn('云台指令失败:', err));
}

// 统一处理鼠标和触摸的按住/松开( Pointer Events )
function bindHold(id, onDown, onUp) {
    const el = $(id);
    let pressed = false;
    el.addEventListener('pointerdown', (e) => {
        e.preventDefault();
        pressed = true;
        try { el.setPointerCapture(e.pointerId); } catch (_) {}
        el.classList.add('active');
        onDown();
    });
    const release = () => {
        if (!pressed) return;
        pressed = false;
        el.classList.remove('active');
        onUp();
    };
    el.addEventListener('pointerup', release);
    el.addEventListener('pointercancel', release);
    // 长按弹出的系统菜单会打断按压, 直接禁掉
    el.addEventListener('contextmenu', (e) => e.preventDefault());
}

bindHold('ptz-up',    () => ptzSend('up', 'down'),    () => ptzSend('up', 'up'));
bindHold('ptz-down',  () => ptzSend('down', 'down'),  () => ptzSend('down', 'up'));
bindHold('ptz-left',  () => ptzSend('left', 'down'),  () => ptzSend('left', 'up'));
bindHold('ptz-right', () => ptzSend('right', 'down'), () => ptzSend('right', 'up'));
bindHold('ptz-talk',  () => ptzSend('talk', 'down'),  () => ptzSend('talk', 'up'));

// ---------- 录制 ----------
// 固件接口: GET /rec?state=start|stop, 成功返回 "1", 失败返回提示文字
let recOn = false;

function applyRec(statusText) {
    const btn = $('rec-toggle');
    btn.textContent = recOn ? '停止录制' : '开始录制';
    btn.classList.toggle('recording', recOn);
    $('rec-status').textContent = statusText !== undefined ? statusText : (recOn ? '录制中' : '就绪');
}

$('rec-toggle').onclick = () => {
    const next = !recOn;
    const btn = $('rec-toggle');
    btn.disabled = true;
    fetch('/rec?state=' + (next ? 'start' : 'stop'))
        .then(r => r.text())
        .then(t => {
            if (t.trim() === '1') {
                recOn = next;
                applyRec(next ? '录制中' : '已停止');
            } else {
                applyRec(t.trim());   // 显示固件返回的失败原因
            }
        })
        .catch(() => applyRec('网络错误'))
        .finally(() => { btn.disabled = false; });
};

// ---------- 初始化: 启动视频流并读取当前状态 ----------
// 任何用户操作都重置"无操作自动关闭"的计时器
['pointermove', 'pointerdown', 'keydown', 'wheel', 'touchstart'].forEach(ev =>
    document.addEventListener(ev, () => {
        if (streaming) resetIdleTimer();
    }, { passive: true })
);
// 回到前台时重新计时(后台期间照常计时, 不中断)
document.addEventListener('visibilitychange', () => {
    if (!document.hidden && streaming) resetIdleTimer();
});

applyStream();
fetch('/status')
    .then(r => r.json())
    .then(s => {
        if (s.framesize !== undefined) $('framesize').value = s.framesize;
        if (s.quality !== undefined) {
            $('quality').value = s.quality;
            $('quality-val').textContent = s.quality;
        }
        if (s.wb_mode !== undefined) $('wb_mode').value = s.wb_mode;
        if (s.led_intensity !== undefined && s.led_intensity >= 0) {
            $('led_intensity').value = s.led_intensity;
            $('led-val').textContent = s.led_intensity;
            ledOn = Number(s.led_intensity) > 0;
            $('led-toggle').textContent = ledOn ? '关' : '开';
        }
        if (s.recording !== undefined) {
            recOn = !!s.recording;
            applyRec();
        }
    })
    .catch(err => console.warn('状态获取失败:', err));
