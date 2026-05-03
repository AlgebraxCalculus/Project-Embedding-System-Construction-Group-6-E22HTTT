import React, { useState, useEffect, useMemo, useRef } from 'react';
import {
  View,
  Text,
  TouchableOpacity,
  Modal,
  FlatList,
  StyleSheet,
  ActivityIndicator,
} from 'react-native';
import { FeedAPI } from '../services/api';
import { createMqttClient } from '../services/mqtt'; // Import MQTT
import { isScheduledAckPayload, extractAckAmount } from '../utils/notificationHelpers'; // Import Helper
import { colors } from '../theme/colors';

const methodLabels = {
  manual: 'Thủ công',
  voice: 'Giọng nói',
  scheduled: 'Theo lịch',
  alert: 'Cảnh báo',
};

export default function NotificationBell() {
  const [open, setOpen] = useState(false);
  const [filter, setFilter] = useState('all');
  const [notifications, setNotifications] = useState([]);
  const [loading, setLoading] = useState(false);
  const [unreadCount, setUnreadCount] = useState(0);
  
  const unreadRef = useRef(unreadCount);
  const openRef = useRef(open);

  // Đồng bộ ref để dùng trong callback của MQTT
  useEffect(() => {
    unreadRef.current = unreadCount;
    openRef.current = open;
  }, [unreadCount, open]);

  // 1. Fetch lịch sử bằng HTTP API
  const fetchNotifications = async () => {
    setLoading(true);
    try {
      const response = await FeedAPI.history(50);
      const feedLogs = response.data?.feedLogs || [];
      const mapped = feedLogs.map((log) => {
        const isSuccess = log.status === 'success';
        const isHopperEmpty = log.status === 'hopper_empty';
        const amount = Number(log.amount ?? 0);
        const target = Number(log.targetAmount ?? amount);
        const amountStr = amount.toFixed(1);
        const targetStr = target.toFixed(0);
        const methodPrefix =
          log.feedType === 'scheduled' ? 'Lịch cho ăn'
          : log.feedType === 'voice' ? 'Giọng nói'
          : 'Cho ăn thủ công';
        const message = isSuccess
          ? `${methodPrefix} ${amountStr}g thành công`
          : isHopperEmpty
          ? `${methodPrefix} thất bại: Hopper rỗng`
          : `${methodPrefix} thất bại (${amountStr}g/${targetStr}g)`;
        return {
          _id: log._id,
          message,
          method: log.feedType,
          amount,
          status: log.status,
          transcript: log.voiceCommand,
          createdAt: log.startTime || log.createdAt,
          read: true,
        };
      });
      setNotifications(mapped);
    } catch (error) {
      console.error('Lỗi khi tải thông báo API:', error);
    } finally {
      setLoading(false);
    }
  };

  // 2. Lắng nghe MQTT thời gian thực
  useEffect(() => {
    fetchNotifications();

    const mqttClient = createMqttClient({
      onAck: (data, rawPayload) => {
        const amount = Number(extractAckAmount(data) ?? 0);
        const target = Number(data.targetAmount ?? amount);
        const isSuccess = data.status === 'success';
        const isHopperEmpty = data.status === 'hopper_empty';
        const isScheduled = isScheduledAckPayload(data);
        const method = isScheduled ? 'scheduled' : (data.mode || 'manual');
        const amountStr = amount.toFixed(1);
        const targetStr = target.toFixed(0);

        const methodPrefix =
          method === 'scheduled' ? 'Lịch cho ăn'
          : method === 'voice' ? 'Giọng nói'
          : 'Cho ăn thủ công';
        const message = isSuccess
          ? `${methodPrefix} ${amountStr}g thành công`
          : isHopperEmpty
          ? `${methodPrefix} thất bại: Hopper rỗng`
          : `${methodPrefix} thất bại (${amountStr}g/${targetStr}g)`;

        const newNotification = {
          _id: Date.now().toString(),
          message,
          method,
          amount,
          status: isSuccess ? 'success' : isHopperEmpty ? 'hopper_empty' : 'failed',
          createdAt: new Date().toISOString(),
          read: false,
        };

        // Thêm thông báo mới lên đầu danh sách
        setNotifications((prev) => [newNotification, ...prev]);

        // Nếu popup đang đóng, tăng biến đếm chưa đọc
        if (!openRef.current) {
          setUnreadCount(prev => prev + 1);
        }
      },
      onAlert: (data) => {
        const isEmpty = data.is_empty === true;
        const newNotification = {
          _id: Date.now().toString(),
          message: isEmpty
            ? 'Hopper rỗng! Vui lòng nạp thêm thức ăn.'
            : 'Hopper đã được nạp đầy. Máy cho ăn sẵn sàng.',
          method: 'alert',
          createdAt: new Date().toISOString(),
          read: false,
        };
        setNotifications((prev) => [newNotification, ...prev]);
        if (!openRef.current) {
          setUnreadCount(prev => prev + 1);
        }
      }
    });

    return () => {
      // Cleanup ngắt kết nối MQTT khi component unmount
      if (mqttClient && typeof mqttClient.end === 'function') {
        mqttClient.end();
      }
    };
  }, []);

  const handleOpen = () => {
    setOpen(true);
    setUnreadCount(0); // Đánh dấu đã đọc khi mở
  };

  const clearNotifications = () => {
    setNotifications([]);
  };

  const filteredItems = useMemo(
    () => (filter === 'all' ? notifications : notifications.filter((item) => item.method === filter)),
    [notifications, filter]
  );

  const formatDate = (dateString) => {
    try {
      const d = new Date(dateString);
      return `${d.getHours().toString().padStart(2, '0')}:${d.getMinutes().toString().padStart(2, '0')} ${d.getDate().toString().padStart(2, '0')}/${(d.getMonth() + 1).toString().padStart(2, '0')}`;
    } catch (error) {
      return dateString;
    }
  };

  const renderItem = ({ item }) => (
    <View style={[styles.notificationItem, item.method === 'alert' && styles.alertItem]}>
      <Text style={[styles.messageText, !item.read && styles.unreadMessage, item.method === 'alert' && styles.alertMessage]}>
        {item.message || 'Hệ thống đã cho ăn'}
      </Text>
      <Text style={styles.subText}>
        {methodLabels[item.method] || 'Thủ công'} • {item.amount ? `${item.amount}g` : '--'} • {formatDate(item.createdAt)}
      </Text>
      {item.transcript ? (
        <Text style={styles.transcriptText}>Lệnh: "{item.transcript}"</Text>
      ) : null}
    </View>
  );

  return (
    <View style={styles.container}>
      <TouchableOpacity style={styles.bellButton} onPress={handleOpen}>
        <Text style={styles.bellIcon}>🔔</Text>
        {unreadCount > 0 && (
          <View style={styles.badge}>
            <Text style={styles.badgeText}>
              {unreadCount > 9 ? '9+' : unreadCount}
            </Text>
          </View>
        )}
      </TouchableOpacity>

      <Modal visible={open} transparent={true} animationType="fade">
        <TouchableOpacity style={styles.modalOverlay} activeOpacity={1} onPress={() => setOpen(false)}>
          <TouchableOpacity activeOpacity={1} style={styles.dropdown}>
            <View style={styles.dropdownHeader}>
              <Text style={styles.dropdownTitle}>Thông báo</Text>
              <TouchableOpacity onPress={clearNotifications}>
                <Text style={styles.clearBtnText}>Xóa tất cả</Text>
              </TouchableOpacity>
            </View>

            <View style={styles.filterContainer}>
              {['all', 'manual', 'voice', 'scheduled'].map((key) => {
                const label = key === 'all' ? 'Tất cả' : methodLabels[key];
                const isActive = filter === key;
                return (
                  <TouchableOpacity
                    key={key}
                    style={[styles.filterBtn, isActive && styles.filterBtnActive]}
                    onPress={() => setFilter(key)}
                  >
                    <Text style={[styles.filterBtnText, isActive && styles.filterBtnTextActive]}>
                      {label}
                    </Text>
                  </TouchableOpacity>
                );
              })}
            </View>

            {loading && notifications.length === 0 ? (
              <ActivityIndicator style={{ margin: 20 }} color={colors.primary} />
            ) : filteredItems.length === 0 ? (
              <Text style={styles.emptyText}>Chưa có thông báo nào</Text>
            ) : (
              <FlatList
                data={filteredItems}
                keyExtractor={(item, index) => item._id || item.id || index.toString()}
                renderItem={renderItem}
                style={styles.list}
              />
            )}
          </TouchableOpacity>
        </TouchableOpacity>
      </Modal>
    </View>
  );
}

const styles = StyleSheet.create({
  container: { position: 'relative', marginRight: 10 },
  bellButton: {
    width: 40, height: 40, backgroundColor: '#f1f5f9',
    borderRadius: 20, alignItems: 'center', justifyContent: 'center',
  },
  bellIcon: { fontSize: 18 },
  badge: {
    position: 'absolute', top: -2, right: -2,
    backgroundColor: '#ef4444', borderRadius: 10,
    minWidth: 18, height: 18, alignItems: 'center',
    justifyContent: 'center', paddingHorizontal: 4,
  },
  badgeText: { color: 'white', fontSize: 10, fontWeight: 'bold' },
  modalOverlay: {
    flex: 1, backgroundColor: 'rgba(0,0,0,0.2)',
    justifyContent: 'flex-start', alignItems: 'flex-end',
  },
  dropdown: {
    backgroundColor: 'white', width: 320, maxHeight: 400,
    marginTop: 60, marginRight: 16, borderRadius: 16,
    padding: 16, elevation: 5,
  },
  dropdownHeader: { flexDirection: 'row', justifyContent: 'space-between', marginBottom: 12 },
  dropdownTitle: { fontSize: 16, fontWeight: 'bold', color: '#0f172a' },
  clearBtnText: { color: '#64748b', fontSize: 13 },
  filterContainer: { flexDirection: 'row', justifyContent: 'space-between', marginBottom: 12 },
  filterBtn: { paddingVertical: 6, paddingHorizontal: 10, borderRadius: 20, borderWidth: 1, borderColor: '#e2e8f0' },
  filterBtnActive: { borderColor: '#0f172a', backgroundColor: '#0f172a' },
  filterBtnText: { fontSize: 11, color: '#0f172a' },
  filterBtnTextActive: { color: '#fff' },
  list: { maxHeight: 300 },
  emptyText: { textAlign: 'center', color: '#94a3b8', marginVertical: 20 },
  notificationItem: { paddingVertical: 12, borderBottomWidth: 1, borderBottomColor: '#f1f5f9' },
  alertItem: { backgroundColor: '#fffbeb' },
  messageText: { fontSize: 14, color: '#0f172a' },
  unreadMessage: { fontWeight: '700' },
  alertMessage: { color: '#b45309' },
  subText: { fontSize: 12, color: '#64748b', marginTop: 4 },
  transcriptText: { fontSize: 11, color: '#94a3b8', marginTop: 4, fontStyle: 'italic' },
});