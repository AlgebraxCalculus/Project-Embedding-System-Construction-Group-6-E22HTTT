import { useCallback, useEffect, useRef, useState } from 'react';
import { createMqttClient } from '../services/mqtt.js';
import { FeedAPI } from '../services/api.js';
import Toast from '../components/Toast.jsx';
import { useNotifications } from '../hooks/useNotifications.jsx';
import { extractAckAmount, isScheduledAckPayload } from '../utils/notificationHelpers.js';

const DEVICE_ID = import.meta.env.VITE_DEVICE_ID || 'petfeeder-feed-node-01';

const ManualFeed = () => {
  const [mqttStatus, setMqttStatus] = useState('offline');
  const [ackMessage, setAckMessage] = useState('');
  const [micStatus, setMicStatus] = useState('idle');
  const [loading, setLoading] = useState(false);
  const [language, setLanguage] = useState('vi-VN'); // 'vi-VN' or 'en-US'
  const [toast, setToast] = useState(null);
  const clientRef = useRef(null);
  const recognitionRef = useRef(null);
  const languageRef = useRef(language);
  const { addNotification } = useNotifications();

  useEffect(() => {
    languageRef.current = language;
  }, [language]);

  const handleMqttAck = useCallback(
    (payload) => {
      if (!payload) return;
      const lang = languageRef.current;
      const ackText = payload.message || JSON.stringify(payload);
      setAckMessage((prev) => {
        const prefix = lang === 'vi-VN' ? '📡 MQTT: ' : '📡 MQTT: ';
        const nextLine = `${prefix}${ackText}`;
        return prev ? `${prev}\n${nextLine}` : nextLine;
      });

      if (isScheduledAckPayload(payload)) {
        const amount = extractAckAmount(payload, 5);
        addNotification({
          method: 'scheduled',
          type: 'info',
          amount,
          message:
            lang === 'vi-VN'
              ? `Lịch cho ăn ${amount}g đã thực thi`
              : `Scheduled feed ${amount}g executed`,
          meta: {
            scheduleId: payload.scheduleId || payload.schedule,
          },
        });
      }
    },
    [addNotification],
  );

  useEffect(() => {
    const client = createMqttClient({
      deviceId: DEVICE_ID,
      onAck: handleMqttAck,
      onStatusChange: (status) => setMqttStatus(status),
    });
    clientRef.current = client;
    return () => client?.end(true);
  }, [handleMqttAck]);

  const handleFeedNow = async () => {
    setLoading(true);
    setAckMessage('Đang gửi lệnh cho ăn...');
    try {
      const { data } = await FeedAPI.manual();
      const feedLog = data.feedLog || {};
      const amount = Number(feedLog.amount ?? 0);
      const target = Number(feedLog.targetAmount ?? 5);
      const isSuccess = feedLog.status === 'success';
      const amountStr = amount.toFixed(1);
      const targetStr = target.toFixed(0);

      const message = isSuccess
        ? (language === 'vi-VN'
            ? `✅ Đã cho ăn ${amountStr}g thành công!`
            : `✅ Successfully fed ${amountStr}g!`)
        : (language === 'vi-VN'
            ? `❌ Cho ăn thất bại: chỉ phát được ${amountStr}g / ${targetStr}g`
            : `❌ Feed failed: only dispensed ${amountStr}g / ${targetStr}g`);
      setAckMessage(message);
      setToast({
        message: isSuccess
          ? (language === 'vi-VN' ? `Đã cho ăn ${amountStr} gram` : `Fed ${amountStr} grams`)
          : (language === 'vi-VN'
              ? `Cho ăn thất bại (${amountStr}g/${targetStr}g)`
              : `Feed failed (${amountStr}g/${targetStr}g)`),
        type: isSuccess ? 'success' : 'error',
      });
      addNotification({
        method: 'manual',
        amount,
        type: isSuccess ? 'success' : 'error',
        message:
          isSuccess
            ? (language === 'vi-VN'
                ? `Cho ăn thủ công ${amountStr}g thành công`
                : `Manual feed ${amountStr}g successful`)
            : (language === 'vi-VN'
                ? `Cho ăn thủ công thất bại (${amountStr}g/${targetStr}g)`
                : `Manual feed failed (${amountStr}g/${targetStr}g)`),
      });
    } catch (err) {
      const errorMsg = err.response?.data?.message || 'Failed to send feed command';
      setAckMessage(`❌ ${errorMsg}`);
      setToast({
        message: errorMsg,
        type: 'error',
      });
      addNotification({
        method: 'manual',
        type: 'error',
        message: errorMsg,
      });
    } finally {
      setLoading(false);
    }
  };

  const handleVoiceFeed = async () => {
    // Dừng listening nếu đang hoạt động
    if (micStatus === 'listening' && recognitionRef.current) {
      recognitionRef.current.stop();
      return;
    }

    // Kiểm tra xem trình duyệt có hỗ trợ Web Speech API không
    const SpeechRecognition = window.SpeechRecognition || window.webkitSpeechRecognition;
    
    if (!SpeechRecognition) {
      setAckMessage('Trình duyệt không hỗ trợ nhận diện giọng nói. Vui lòng dùng Chrome hoặc Edge.');
      return;
    }

    try {
      const recognition = new SpeechRecognition();
      recognition.lang = language;
      recognition.continuous = false;
      recognition.interimResults = false;
      recognition.maxAlternatives = 1;

      recognition.onstart = () => {
        setMicStatus('listening');
        if (language === 'vi-VN') {
          setAckMessage('🎙️ Đang lắng nghe... Nói "cho ăn" (mặc định 5g) hoặc "cho ăn 200 gram"');
        } else {
          setAckMessage('🎙️ Listening... Say "feed" (default 5g) or "feed 200 grams"');
        }
      };

      recognition.onresult = async (event) => {
        const transcript = event.results[0][0].transcript.trim();
        setMicStatus('processing');
        
        // Validate trước khi gửi (chỉ cần trigger phrase, số lượng là optional)
        const lowerText = transcript.toLowerCase();
        // Check for Vietnamese trigger
        const hasViTrigger = lowerText.includes('cho ăn') || lowerText.includes('cho an');
        // Check for English trigger
        const hasEnTrigger = lowerText.includes('feed') || lowerText.includes('give food') || lowerText.includes('dispense');
        const hasTrigger = hasViTrigger || hasEnTrigger;

        // LUÔN hiển thị transcript đã nhận diện được
        if (language === 'vi-VN') {
          setAckMessage(`🎙️ Đã nghe: "${transcript}"`);
        } else {
          setAckMessage(`🎙️ Heard: "${transcript}"`);
        }

        if (!hasTrigger) {
          if (language === 'vi-VN') {
            setAckMessage(`🎙️ Đã nghe: "${transcript}"\n⚠️ Không tìm thấy cụm kích hoạt. Vui lòng nói: "cho ăn" (mặc định 5g) hoặc "cho ăn 200 gram"`);
          } else {
            setAckMessage(`🎙️ Heard: "${transcript}"\n⚠️ No trigger phrase found. Please say: "feed" (default 5g) or "feed 200 grams"`);
          }
          setToast({
            message: language === 'vi-VN' 
              ? `Không tìm thấy lệnh trong: "${transcript}"`
              : `No command found in: "${transcript}"`,
            type: 'warning',
          });
          setMicStatus('idle');
          return;
        }
        
        // Kiểm tra số lượng
        const amountMatch = transcript.match(/(\d+)\s*(gram|gr|g|grams)\b/i);
        const hasAmount = !!amountMatch;
        const detectedAmount = amountMatch ? parseInt(amountMatch[1], 10) : 5;
        
        // Hiển thị thông tin về số lượng
        if (hasAmount) {
          if (language === 'vi-VN') {
            setAckMessage(`🎙️ Đã nghe: "${transcript}"\n📊 Nhận diện: ${detectedAmount}g\n⏳ Đang gửi lệnh...`);
          } else {
            setAckMessage(`🎙️ Heard: "${transcript}"\n📊 Detected: ${detectedAmount}g\n⏳ Sending command...`);
          }
        } else {
          if (language === 'vi-VN') {
            setAckMessage(`🎙️ Đã nghe: "${transcript}"\n📊 Không có số lượng, dùng mặc định: 5g\n⏳ Đang gửi lệnh...`);
          } else {
            setAckMessage(`🎙️ Heard: "${transcript}"\n📊 No amount, using default: 5g\n⏳ Sending command...`);
          }
        }

        try {
          setLoading(true);
          const { data: feedData } = await FeedAPI.voice(transcript);
          const feedLog = feedData.feedLog || {};
          const feedAmount = Number(feedLog.amount ?? feedData.parsedAmount ?? detectedAmount);
          const target = Number(feedLog.targetAmount ?? detectedAmount);
          const isSuccess = feedLog.status === 'success';
          const amountStr = feedAmount.toFixed(1);
          const targetStr = target.toFixed(0);

          if (isSuccess) {
            if (language === 'vi-VN') {
              setAckMessage(`🎙️ Đã nghe: "${transcript}"\n✅ Đã cho ăn ${amountStr}g thành công!`);
            } else {
              setAckMessage(`🎙️ Heard: "${transcript}"\n✅ Successfully fed ${amountStr}g!`);
            }
          } else {
            if (language === 'vi-VN') {
              setAckMessage(`🎙️ Đã nghe: "${transcript}"\n❌ Cho ăn thất bại: chỉ phát được ${amountStr}g/${targetStr}g`);
            } else {
              setAckMessage(`🎙️ Heard: "${transcript}"\n❌ Feed failed: only dispensed ${amountStr}g/${targetStr}g`);
            }
          }

          setToast({
            message: isSuccess
              ? (language === 'vi-VN'
                  ? `Đã cho ăn ${amountStr} gram\n(Lệnh: "${transcript}")`
                  : `Fed ${amountStr} grams\n(Command: "${transcript}")`)
              : (language === 'vi-VN'
                  ? `Cho ăn thất bại (${amountStr}g/${targetStr}g)\n(Lệnh: "${transcript}")`
                  : `Feed failed (${amountStr}g/${targetStr}g)\n(Command: "${transcript}")`),
            type: isSuccess ? 'success' : 'error',
          });
          addNotification({
            method: 'voice',
            amount: feedAmount,
            transcript,
            type: isSuccess ? 'success' : 'error',
            message:
              isSuccess
                ? (language === 'vi-VN'
                    ? `Giọng nói: cho ăn ${amountStr}g`
                    : `Voice feed ${amountStr}g`)
                : (language === 'vi-VN'
                    ? `Giọng nói thất bại (${amountStr}g/${targetStr}g)`
                    : `Voice feed failed (${amountStr}g/${targetStr}g)`),
          });
        } catch (err) {
          console.error('Voice feed error:', err);
          const errorMsg = err.response?.data?.error || err.response?.data?.message || 'Gửi lệnh thất bại';
          if (language === 'vi-VN') {
            setAckMessage(`🎙️ Đã nghe: "${transcript}"\n❌ ${errorMsg}`);
          } else {
            setAckMessage(`🎙️ Heard: "${transcript}"\n❌ ${errorMsg}`);
          }
          setToast({
            message: `${errorMsg}\n(Lệnh: "${transcript}")`,
            type: 'error',
          });
          addNotification({
            method: 'voice',
            type: 'error',
            transcript,
            message: errorMsg,
          });
        } finally {
          setLoading(false);
          setMicStatus('idle');
        }
      };

      recognition.onerror = (event) => {
        console.error('Speech recognition error:', event.error);
        let errorMessage = 'Lỗi nhận diện giọng nói';
        
        if (event.error === 'no-speech') {
          errorMessage = 'Không nghe thấy giọng nói. Vui lòng thử lại.';
        } else if (event.error === 'audio-capture') {
          errorMessage = 'Không thể truy cập microphone. Vui lòng kiểm tra quyền.';
        } else if (event.error === 'not-allowed') {
          errorMessage = 'Quyền truy cập microphone bị từ chối.';
        }
        
        setAckMessage(errorMessage);
        setMicStatus('idle');
      };

      recognition.onend = () => {
        if (micStatus === 'listening') {
          setMicStatus('idle');
        }
      };

      recognitionRef.current = recognition;
      recognition.start();
    } catch (error) {
      console.error('Error starting speech recognition:', error);
      setAckMessage('Không thể khởi động nhận diện giọng nói.');
      setMicStatus('idle');
    }
  };

  return (
    <div className="page">
      <div className="page__header">
        <div>
          <h2>Feed Now</h2>
          <p>Instant feeding or via voice command</p>
        </div>
        <p className="badge">MQTT: {mqttStatus}</p>
      </div>
      <section className="grid grid--2">
        <div className="card">
          <h3>Manual Feed</h3>
          <p>Dispense food</p>
          <button className="btn btn--primary btn--lg" type="button" onClick={handleFeedNow} disabled={loading}>
            {loading ? 'Sending...' : 'Feed Now'}
          </button>
        </div>
        <div className="card">
          <h3>Feed by Voice</h3>
          <p>Say "cho ăn" or "cho ăn 200 gram", "feed" or "feed 200 grams"</p>
          <div style={{ marginBottom: '1rem', display: 'flex', gap: '0.5rem', alignItems: 'center' }}>
            <label htmlFor="language-select" style={{ fontSize: '0.9rem' }}>Language:</label>
            <select
              id="language-select"
              value={language}
              onChange={(e) => setLanguage(e.target.value)}
              disabled={micStatus === 'listening' || micStatus === 'processing'}
              style={{
                padding: '0.5rem',
                borderRadius: '0.5rem',
                border: '1px solid #e0e7ff',
                fontSize: '0.9rem',
                cursor: 'pointer',
              }}
            >
              <option value="vi-VN">Tiếng Việt</option>
              <option value="en-US">English</option>
            </select>
          </div>
          <button
            className={`voice-button ${micStatus === 'listening' ? 'voice-button--listening' : ''}`}
            type="button"
            onClick={handleVoiceFeed}
            disabled={loading}
          >
            <span className="voice-button__dot" aria-hidden />
            <span className="voice-button__label">
              {micStatus === 'listening' 
                ? (language === 'vi-VN' ? 'Đang nghe...' : 'Listening...')
                : micStatus === 'processing' 
                ? (language === 'vi-VN' ? 'Đang xử lý...' : 'Processing...')
                : (language === 'vi-VN' ? 'Nhấn để nói' : 'Click to speak')}
            </span>
          </button>
          <small>
            {language === 'vi-VN' 
              ? 'Trình duyệt sẽ xin quyền sử dụng microphone.'
              : 'Browser will ask for microphone permission.'}
          </small>
        </div>
      </section>
      
      {/* Hiển thị thông tin chi tiết */}
      <section style={{ marginTop: '2rem' }}>
        {ackMessage && (
          <div 
            className="alert alert--info" 
            style={{ 
              whiteSpace: 'pre-line',
              lineHeight: '1.6',
              fontSize: '0.95rem',
            }}
          >
            {ackMessage}
          </div>
        )}
      </section>
      
      {/* Toast Notification */}
      {toast && (
        <Toast
          message={toast.message}
          type={toast.type}
          onClose={() => setToast(null)}
          duration={5000}
        />
      )}
    </div>
  );
};

export default ManualFeed;


