//! PipeWire capture stream named `Helix microphone level` (C++ `input_meter.cpp`).

use std::convert::TryInto;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use pipewire as pw;
use pw::properties::properties;
use pw::spa::param::audio::{AudioFormat, AudioInfoRaw};
use pw::spa::param::ParamType;
use pw::spa::pod::serialize::PodSerializer;
use pw::spa::pod::{Object, Pod, Value};
use pw::spa::utils::{Direction, SpaTypes};
use pw::loop_::Timeout;
use pw::stream::{StreamBox, StreamFlags, StreamState};

pub fn input_peak_from_bytes(bytes: &[u8]) -> f64 {
    let mut peak = 0.0f64;
    let mut offset = 0;
    while offset + 4 <= bytes.len() {
        let sample = f32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap());
        if sample.is_finite() {
            peak = peak.max(sample.abs() as f64);
        }
        offset += 4;
    }
    peak.clamp(0.0, 1.0)
}

struct CaptureShared {
    peak: Mutex<f64>,
    streaming: AtomicBool,
    stop: AtomicBool,
}

pub struct PipeWireInputCapture {
    shared: Arc<CaptureShared>,
    thread: Option<std::thread::JoinHandle<()>>,
}

impl PipeWireInputCapture {
    pub fn start(source_id: u32) -> Option<Self> {
        let shared = Arc::new(CaptureShared {
            peak: Mutex::new(0.0),
            streaming: AtomicBool::new(false),
            stop: AtomicBool::new(false),
        });
        let thread_shared = Arc::clone(&shared);
        let thread = std::thread::spawn(move || {
            if let Err(error) = run_capture(source_id, thread_shared) {
                tracing::warn!(%error, source_id, "microphone level capture failed");
            }
        });
        Some(Self {
            shared,
            thread: Some(thread),
        })
    }

    pub fn peak(&self) -> f64 {
        *self.shared.peak.lock().expect("peak")
    }

    pub fn available(&self) -> bool {
        self.shared.streaming.load(Ordering::Relaxed)
    }
}

impl Drop for PipeWireInputCapture {
    fn drop(&mut self) {
        self.shared.stop.store(true, Ordering::Relaxed);
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

fn run_capture(source_id: u32, shared: Arc<CaptureShared>) -> Result<(), pw::Error> {
    pw::init();

    let mainloop = pw::main_loop::MainLoopRc::new(None)?;
    let context = pw::context::ContextRc::new(&mainloop, None)?;
    let core = context.connect_rc(None)?;

    let peak_state = Arc::clone(&shared);
    let streaming_state = Arc::clone(&shared);

    let stream = StreamBox::new(
        &core,
        "Helix microphone level",
        properties! {
            *pw::keys::MEDIA_TYPE => "Audio",
            *pw::keys::MEDIA_CATEGORY => "Capture",
            *pw::keys::MEDIA_ROLE => "Production",
            *pw::keys::APP_NAME => "Helix Settings microphone test",
        },
    )?;

    let _listener = stream
        .add_local_listener_with_user_data(())
        .state_changed(move |_, (), _old, new| {
            let streaming = new == StreamState::Streaming;
            streaming_state.streaming.store(streaming, Ordering::Relaxed);
            if !streaming {
                if let Ok(mut peak) = peak_state.peak.lock() {
                    *peak = 0.0;
                }
            }
        })
        .process({
            let peak_state = Arc::clone(&shared);
            move |stream, ()| {
                let Some(mut buffer) = stream.dequeue_buffer() else {
                    return;
                };
                let mut peak = 0.0f64;
                for data in buffer.datas_mut() {
                    let (offset, size) = {
                        let chunk = data.chunk();
                        (chunk.offset() as usize, chunk.size() as usize)
                    };
                    let Some(bytes) = data.data() else {
                        continue;
                    };
                    if offset > bytes.len() {
                        continue;
                    }
                    let end = (offset + size).min(bytes.len());
                    peak = peak.max(input_peak_from_bytes(&bytes[offset..end]));
                }
                if let Ok(mut stored) = peak_state.peak.lock() {
                    *stored = peak;
                }
            }
        })
        .register()?;

    let mut audio_info = AudioInfoRaw::new();
    audio_info.set_format(AudioFormat::F32LE);
    audio_info.set_rate(48_000);
    audio_info.set_channels(1);
    let mut position = [0u32; pw::spa::sys::SPA_AUDIO_MAX_CHANNELS as usize];
    position[0] = pw::spa::sys::SPA_AUDIO_CHANNEL_MONO;
    audio_info.set_position(position);

    let obj = Object {
        type_: SpaTypes::ObjectParamFormat.as_raw(),
        id: ParamType::EnumFormat.as_raw(),
        properties: audio_info.into(),
    };
    let values: Vec<u8> = PodSerializer::serialize(
        std::io::Cursor::new(Vec::new()),
        &Value::Object(obj),
    )
    .unwrap()
    .0
    .into_inner();

    let mut params = [Pod::from_bytes(&values).unwrap()];

    stream.connect(
        Direction::Input,
        Some(source_id),
        StreamFlags::AUTOCONNECT | StreamFlags::MAP_BUFFERS | StreamFlags::DONT_RECONNECT,
        &mut params,
    )?;

    while !shared.stop.load(Ordering::Relaxed) {
        mainloop
            .loop_()
            .iterate(Timeout::Finite(Duration::from_millis(10)));
    }

    Ok(())
}
