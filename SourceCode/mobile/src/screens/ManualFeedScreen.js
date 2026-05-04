import { useCallback, useEffect, useRef, useState } from 'react';
import {
  ActivityIndicator,
  ScrollView,
  StyleSheet,
  Text,
  TouchableOpacity,
  View,
} from 'react-native';
import { Audio } from 'expo-av';
import { SafeAreaView } from 'react-native-safe-area-context';
import { FeedAPI, SpeechAPI } from '../services/api';
import { createMqttClient } from '../services/mqtt';
import { colors } from '../theme/colors';
import { isScheduledAckPayload, extractAckAmount } from '../utils/notificationHelpers';

const DEVICE_ID = process.env.EXPO_PUBLIC_DEVICE_ID || 'petfeeder-feed-node-01';

const MqttDot = ({ status }) => {
  const dotColor =
    status === 'online' ? colors.success : status === 'reconnecting' ? colors.warning : '#aaa';
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
      <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: dotColor }} />
      <Text style={{ fontSize: 12, color: colors.textSecondary }}>MQTT: {status}</Text>
    </View>
  );
};

export default function ManualFeedScreen() {
  const [mqttStatus, setMqttStatus] = useState('offline');
  const [ackMessage, setAckMessage] = useState('');
  const [loading, setLoading] = useState(false);
  const [micStatus, setMicStatus] = useState('idle'); // idle | recording | processing
  const clientRef = useRef(null);
  const recordingRef = useRef(null);

  const handleMqttAck = useCallback((payload) => {
    if (!payload) return;
    const ackText = payload.message || JSON.stringify(payload);
    setAckMessage((prev) => {
      const line = `📡 MQTT: ${ackText}`;
      return prev ? `${prev}\n${line}` : line;
    });
  }, []);

  useEffect(() => {
    const client = createMqttClient({
      deviceId: DEVICE_ID,
      onAck: handleMqttAck,
      onStatusChange: setMqttStatus,
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
      const isHopperEmpty = feedLog.status === 'hopper_empty';
      const amountStr = amount.toFixed(1);
      const targetStr = target.toFixed(0);

      setAckMessage(
        isSuccess
          ? `✅ Đã cho ăn ${amountStr}g thành công!`
          : isHopperEmpty
          ? `⚠️ Không thể cho ăn: Hopper rỗng, vui lòng nạp thêm thức ăn`
          : `❌ Cho ăn thất bại: chỉ phát được ${amountStr}g / ${targetStr}g`
      );
    } catch (err) {
      const msg = err.response?.data?.message || 'Gửi lệnh thất bại';
      setAckMessage(`❌ ${msg}`);
    } finally {
      setLoading(false);
    }
  };

  const startRecording = async () => {
    try {
      const { granted } = await Audio.requestPermissionsAsync();
      if (!granted) {
        setAckMessage('Quyền truy cập micro bị từ chối');
        return;
      }

      await Audio.setAudioModeAsync({
        allowsRecordingIOS: true,
        playsInSilentModeIOS: true,
      });

      const { recording } = await Audio.Recording.createAsync(
        Audio.RecordingOptionsPresets.HIGH_QUALITY,
      );
      recordingRef.current = recording;
      setMicStatus('recording');
      setAckMessage('Dang lang nghe... Noi "cho an" hoac "cho an 200 gram"');
    } catch (err) {
      console.warn('Start recording error:', err);
      setAckMessage('Khong the bat micro: ' + err.message);
    }
  };

  const stopAndSend = async () => {
    if (!recordingRef.current) return;

    setMicStatus('processing');
    setAckMessage('Dang xu ly giong noi...');

    try {
      await recordingRef.current.stopAndUnloadAsync();
      const uri = recordingRef.current.getURI();
      recordingRef.current = null;

      await Audio.setAudioModeAsync({ allowsRecordingIOS: false });

      // Step 1: Send audio to speech-to-text module
      const { data: sttResult } = await SpeechAPI.transcribe(uri);

      if (!sttResult.success || !sttResult.text) {
        setAckMessage('Khong nhan dien duoc giong noi. Hay thu lai.');
        setMicStatus('idle');
        return;
      }

      const text = sttResult.text.trim();
      setAckMessage(`Nhan dien: "${text}"\nDang gui lenh...`);

      // Step 2: Send transcribed text to backend voice endpoint
      const { data } = await FeedAPI.voice(text);
      const feedLog = data.feedLog || {};
      const amount = Number(feedLog.amount ?? data.parsedAmount ?? 0);
      const target = Number(feedLog.targetAmount ?? data.parsedAmount ?? 5);
      const isSuccess = feedLog.status === 'success';
      const isHopperEmpty = feedLog.status === 'hopper_empty';
      const amountStr = amount.toFixed(1);
      const targetStr = target.toFixed(0);

      setAckMessage(
        isSuccess
          ? `Lệnh: "${text}"\nĐã cho ăn ${amountStr}g thành công!`
          : isHopperEmpty
          ? `Lệnh: "${text}"\n⚠️ Không thể cho ăn: Hopper rỗng`
          : `Lệnh: "${text}"\nCho ăn thất bại: chỉ phát được ${amountStr}g / ${targetStr}g`
      );
    } catch (err) {
      const msg = err.response?.data?.error || err.response?.data?.message || err.message || 'Gui lenh that bai';
      setAckMessage(`Loi: ${msg}`);
    } finally {
      setMicStatus('idle');
    }
  };

  const handleMicPress = () => {
    if (micStatus === 'idle') startRecording();
    else if (micStatus === 'recording') stopAndSend();
  };

  return (
    <SafeAreaView style={s.safe}>
      <ScrollView style={s.scroll} keyboardShouldPersistTaps="handled">
        {/* Header */}
        <View style={s.header}>
          <View>
            <Text style={s.headerTitle}>Feed Now</Text>
            <Text style={s.headerSub}>Cho ăn ngay hoặc qua lệnh giọng nói</Text>
          </View>
          <MqttDot status={mqttStatus} />
        </View>

        {/* Manual Feed Card */}
        <View style={s.card}>
          <Text style={s.cardTitle}>Cho ăn thủ công</Text>
          <Text style={s.cardSub}>Phát thức ăn ngay lập tức (mặc định 5g)</Text>
          <TouchableOpacity
            style={[s.feedBtn, loading && s.feedBtnDisabled]}
            onPress={handleFeedNow}
            disabled={loading}
          >
            {loading ? (
              <ActivityIndicator color="#fff" />
            ) : (
              <>
                <Text style={s.feedBtnIcon}>🍽️</Text>
                <Text style={s.feedBtnText}>Cho ăn ngay</Text>
              </>
            )}
          </TouchableOpacity>
        </View>

        {/* Voice Command Card */}
        <View style={s.card}>
          <Text style={s.cardTitle}>{`L\u1EC7nh gi\u1ECDng n\u00F3i`}</Text>
          <Text style={s.cardSub}>
            {`Nh\u1EA5n n\u00FAt micro, n\u00F3i: "cho \u0103n" (m\u1EB7c \u0111\u1ECBnh 5g) ho\u1EB7c "cho \u0103n 200 gram"`}
          </Text>

          <TouchableOpacity
            style={[
              s.micBtn,
              micStatus === 'recording' && s.micBtnRecording,
              micStatus === 'processing' && s.feedBtnDisabled,
            ]}
            onPress={handleMicPress}
            disabled={micStatus === 'processing'}
          >
            {micStatus === 'processing' ? (
              <ActivityIndicator color="#fff" />
            ) : (
              <Text style={s.micBtnIcon}>
                {micStatus === 'recording' ? '\u23F9' : '\uD83C\uDF99\uFE0F'}
              </Text>
            )}
          </TouchableOpacity>

          <Text style={s.micLabel}>
            {micStatus === 'idle' && 'Nhan de noi'}
            {micStatus === 'recording' && 'Dang ghi am... Nhan de dung'}
            {micStatus === 'processing' && 'Dang xu ly...'}
          </Text>

          <Text style={s.hint}>
            Cu phap: "cho an [so] gram" — vi du: "cho an 100 gram"
          </Text>
        </View>

        {/* ACK / Result */}
        {ackMessage ? (
          <View style={s.card}>
            <Text style={s.cardTitle}>Kết quả</Text>
            <Text style={s.ackText}>{ackMessage}</Text>
          </View>
        ) : null}

        <View style={{ height: 24 }} />
      </ScrollView>
    </SafeAreaView>
  );
}

const s = StyleSheet.create({
  safe: { flex: 1, backgroundColor: colors.background },
  scroll: { flex: 1 },
  header: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'flex-start',
    paddingHorizontal: 20,
    paddingTop: 16,
    paddingBottom: 8,
  },
  headerTitle: { fontSize: 22, fontWeight: '800', color: colors.textPrimary },
  headerSub: { fontSize: 13, color: colors.textSecondary, marginTop: 2 },
  card: {
    backgroundColor: colors.surface,
    borderRadius: 16,
    marginHorizontal: 16,
    marginBottom: 16,
    padding: 18,
    shadowColor: colors.dark,
    shadowOffset: { width: 0, height: 2 },
    shadowOpacity: 0.06,
    shadowRadius: 8,
    elevation: 3,
  },
  cardTitle: { fontSize: 16, fontWeight: '700', color: colors.textPrimary, marginBottom: 4 },
  cardSub: { fontSize: 13, color: colors.textSecondary, marginBottom: 16 },
  feedBtn: {
    flexDirection: 'row',
    justifyContent: 'center',
    alignItems: 'center',
    gap: 10,
    backgroundColor: colors.primary,
    borderRadius: 14,
    paddingVertical: 16,
  },
  feedBtnDisabled: { opacity: 0.6 },
  feedBtnIcon: { fontSize: 22 },
  feedBtnText: { color: '#fff', fontSize: 17, fontWeight: '700' },
  micBtn: {
    width: 80,
    height: 80,
    borderRadius: 40,
    backgroundColor: colors.primary,
    justifyContent: 'center',
    alignItems: 'center',
    alignSelf: 'center',
    marginBottom: 8,
  },
  micBtnRecording: {
    backgroundColor: '#e53e3e',
  },
  micBtnIcon: { fontSize: 32, color: '#fff' },
  micLabel: {
    textAlign: 'center',
    fontSize: 14,
    color: colors.textSecondary,
    marginBottom: 12,
  },
  hint: { fontSize: 12, color: colors.textSecondary },
  ackText: {
    fontSize: 14,
    color: colors.textPrimary,
    lineHeight: 22,
    backgroundColor: colors.background,
    padding: 12,
    borderRadius: 10,
  },
});
