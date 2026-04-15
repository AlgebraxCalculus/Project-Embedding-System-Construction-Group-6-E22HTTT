import { useEffect, useState } from 'react';
import {
  ActivityIndicator,
  Alert,
  FlatList,
  Modal,
  RefreshControl,
  ScrollView,
  StyleSheet,
  Switch,
  Text,
  TextInput,
  TouchableOpacity,
  View,
} from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';
import { ScheduleAPI } from '../services/api';
import { colors } from '../theme/colors';

const DAY_OPTIONS = [
  { name: 'T2', value: 1 },
  { name: 'T3', value: 2 },
  { name: 'T4', value: 3 },
  { name: 'T5', value: 4 },
  { name: 'T6', value: 5 },
  { name: 'T7', value: 6 },
  { name: 'CN', value: 0 },
];

const DAY_FULL = { 0: 'CN', 1: 'T2', 2: 'T3', 3: 'T4', 4: 'T5', 5: 'T6', 6: 'T7' };

const EMPTY_FORM = {
  name: 'Lịch cho ăn',
  time: '08:00',
  daysOfWeek: [1],
  amount: '50',
  isActive: true,
};

export default function ScheduleScreen() {
  const [schedules, setSchedules] = useState([]);
  const [loading, setLoading] = useState(false);
  const [refreshing, setRefreshing] = useState(false);
  const [error, setError] = useState('');
  const [modalVisible, setModalVisible] = useState(false);
  const [form, setForm] = useState(EMPTY_FORM);
  const [editingId, setEditingId] = useState(null);
  const [saving, setSaving] = useState(false);

  const loadSchedules = async () => {
    setLoading(true);
    setError('');
    try {
      const { data } = await ScheduleAPI.list();
      setSchedules(data.schedules || []);
    } catch (err) {
      setError(err.response?.data?.message || 'Không thể tải lịch cho ăn');
    } finally {
      setLoading(false);
    }
  };

  const onRefresh = async () => {
    setRefreshing(true);
    await loadSchedules();
    setRefreshing(false);
  };

  useEffect(() => { loadSchedules(); }, []);

  const openCreate = () => {
    setForm(EMPTY_FORM);
    setEditingId(null);
    setModalVisible(true);
  };

  const openEdit = (entry) => {
    setForm({
      name: entry.name || 'Lịch cho ăn',
      time: entry.time || '08:00',
      daysOfWeek: Array.isArray(entry.daysOfWeek) && entry.daysOfWeek.length ? entry.daysOfWeek : [1],
      amount: (entry.amount ?? 50).toString(),
      isActive: Boolean(entry.isActive),
    });
    setEditingId(entry.id || entry._id);
    setModalVisible(true);
  };

  const handleSave = async () => {
    if (!form.time.match(/^\d{2}:\d{2}$/)) {
      Alert.alert('Lỗi', 'Định dạng giờ không hợp lệ. Hãy nhập theo HH:mm (ví dụ: 08:30)');
      return;
    }
    if (!form.daysOfWeek.length) {
      Alert.alert('Lỗi', 'Vui lòng chọn ít nhất 1 ngày trong tuần');
      return;
    }
    setSaving(true);
    try {
      const payload = { ...form, amount: Number(form.amount) };
      if (editingId) {
        await ScheduleAPI.update(editingId, payload);
      } else {
        await ScheduleAPI.create(payload);
      }
      setModalVisible(false);
      loadSchedules();
    } catch (err) {
      Alert.alert('Lỗi', err.response?.data?.message || 'Lưu thất bại');
    } finally {
      setSaving(false);
    }
  };

  const handleDelete = (entry) => {
    Alert.alert('Xác nhận xóa', `Xóa lịch "${entry.name}"?`, [
      { text: 'Hủy', style: 'cancel' },
      {
        text: 'Xóa',
        style: 'destructive',
        onPress: async () => {
          try {
            await ScheduleAPI.remove(entry.id || entry._id);
            loadSchedules();
          } catch (err) {
            Alert.alert('Lỗi', err.response?.data?.message || 'Xóa thất bại');
          }
        },
      },
    ]);
  };

  const toggleDay = (dayVal) => {
    setForm((prev) => {
      const exists = prev.daysOfWeek.includes(dayVal);
      return {
        ...prev,
        daysOfWeek: exists
          ? prev.daysOfWeek.filter((d) => d !== dayVal)
          : [...prev.daysOfWeek, dayVal],
      };
    });
  };

  const renderSchedule = ({ item }) => (
    <View style={s.scheduleCard}>
      <View style={s.scheduleTop}>
        <View style={{ flex: 1 }}>
          <Text style={s.scheduleName}>{item.name}</Text>
          <Text style={s.scheduleTime}>⏰ {item.time}</Text>
          <Text style={s.scheduleDays}>
            📅 {Array.isArray(item.daysOfWeek) && item.daysOfWeek.length
              ? item.daysOfWeek.map((d) => DAY_FULL[d] || d).join(', ')
              : '—'}
          </Text>
          <Text style={s.scheduleAmount}>🍽️ {item.amount} g</Text>
        </View>
        <View style={[s.statusBadge, item.isActive ? s.statusActive : s.statusPaused]}>
          <Text style={[s.statusText, item.isActive ? s.statusActiveText : s.statusPausedText]}>
            {item.isActive ? 'Đang bật' : 'Tạm dừng'}
          </Text>
        </View>
      </View>
      <View style={s.scheduleActions}>
        <TouchableOpacity style={s.editBtn} onPress={() => openEdit(item)}>
          <Text style={s.editBtnText}>Sửa</Text>
        </TouchableOpacity>
        <TouchableOpacity style={s.deleteBtn} onPress={() => handleDelete(item)}>
          <Text style={s.deleteBtnText}>Xóa</Text>
        </TouchableOpacity>
      </View>
    </View>
  );

  return (
    <SafeAreaView style={s.safe}>
      {/* Header */}
      <View style={s.header}>
        <View>
          <Text style={s.headerTitle}>Scheduled Feeding</Text>
          <Text style={s.headerSub}>Cài đặt lịch cho ăn tự động</Text>
        </View>
        <View style={{ flexDirection: 'row', gap: 8 }}>
          <TouchableOpacity style={s.ghostBtn} onPress={loadSchedules} disabled={loading}>
            <Text style={s.ghostBtnText}>Làm mới</Text>
          </TouchableOpacity>
          <TouchableOpacity style={s.addBtn} onPress={openCreate}>
            <Text style={s.addBtnText}>+ Thêm</Text>
          </TouchableOpacity>
        </View>
      </View>

      {error ? <Text style={s.errorText}>{error}</Text> : null}

      {loading && !refreshing ? (
        <ActivityIndicator color={colors.primary} style={{ marginTop: 32 }} />
      ) : (
        <FlatList
          data={schedules}
          keyExtractor={(item) => (item.id || item._id || '').toString()}
          renderItem={renderSchedule}
          contentContainerStyle={{ padding: 16, paddingBottom: 32 }}
          refreshControl={
            <RefreshControl refreshing={refreshing} onRefresh={onRefresh} tintColor={colors.primary} />
          }
          ListEmptyComponent={
            <View style={s.emptyBox}>
              <Text style={s.emptyIcon}>🕒</Text>
              <Text style={s.emptyText}>Chưa có lịch nào.</Text>
              <Text style={s.emptyHint}>Nhấn "+ Thêm" để tạo lịch mới.</Text>
            </View>
          }
        />
      )}

      {/* Modal Form */}
      <Modal visible={modalVisible} animationType="slide" presentationStyle="pageSheet">
        <SafeAreaView style={{ flex: 1, backgroundColor: colors.background }}>
          <ScrollView contentContainerStyle={{ padding: 20 }} keyboardShouldPersistTaps="handled">
            {/* Modal Header */}
            <View style={s.modalHeader}>
              <Text style={s.modalTitle}>{editingId ? 'Sửa lịch' : 'Tạo lịch mới'}</Text>
              <TouchableOpacity onPress={() => setModalVisible(false)}>
                <Text style={{ fontSize: 26, color: colors.textSecondary }}>✕</Text>
              </TouchableOpacity>
            </View>

            {/* Name */}
            <Text style={s.fieldLabel}>Tên lịch</Text>
            <TextInput
              style={s.input}
              value={form.name}
              onChangeText={(v) => setForm((p) => ({ ...p, name: v }))}
              placeholder="Tên lịch cho ăn"
              placeholderTextColor={colors.textSecondary}
            />

            {/* Time */}
            <Text style={s.fieldLabel}>Giờ (HH:mm)</Text>
            <TextInput
              style={s.input}
              value={form.time}
              onChangeText={(v) => setForm((p) => ({ ...p, time: v }))}
              placeholder="08:00"
              placeholderTextColor={colors.textSecondary}
              keyboardType="numbers-and-punctuation"
            />

            {/* Amount */}
            <Text style={s.fieldLabel}>Lượng thức ăn (g)</Text>
            <TextInput
              style={s.input}
              value={form.amount}
              onChangeText={(v) => setForm((p) => ({ ...p, amount: v }))}
              placeholder="50"
              placeholderTextColor={colors.textSecondary}
              keyboardType="number-pad"
            />

            {/* Days */}
            <Text style={s.fieldLabel}>Ngày trong tuần</Text>
            <View style={s.chipGroup}>
              {DAY_OPTIONS.map((day) => (
                <TouchableOpacity
                  key={day.value}
                  style={[s.chip, form.daysOfWeek.includes(day.value) && s.chipActive]}
                  onPress={() => toggleDay(day.value)}
                >
                  <Text
                    style={[s.chipText, form.daysOfWeek.includes(day.value) && s.chipTextActive]}
                  >
                    {day.name}
                  </Text>
                </TouchableOpacity>
              ))}
            </View>

            {/* Active Toggle */}
            <View style={s.switchRow}>
              <Text style={s.fieldLabel}>Kích hoạt lịch</Text>
              <Switch
                value={form.isActive}
                onValueChange={(v) => setForm((p) => ({ ...p, isActive: v }))}
                trackColor={{ false: colors.border, true: colors.primary }}
                thumbColor={colors.surface}
              />
            </View>

            {/* Submit */}
            <TouchableOpacity
              style={[s.saveBtn, saving && { opacity: 0.6 }]}
              onPress={handleSave}
              disabled={saving}
            >
              {saving ? (
                <ActivityIndicator color="#fff" />
              ) : (
                <Text style={s.saveBtnText}>{editingId ? 'Lưu thay đổi' : 'Thêm lịch'}</Text>
              )}
            </TouchableOpacity>

            {editingId && (
              <TouchableOpacity
                style={s.cancelBtn}
                onPress={() => { setModalVisible(false); setEditingId(null); setForm(EMPTY_FORM); }}
              >
                <Text style={s.cancelBtnText}>Hủy</Text>
              </TouchableOpacity>
            )}
          </ScrollView>
        </SafeAreaView>
      </Modal>
    </SafeAreaView>
  );
}

const s = StyleSheet.create({
  safe: { flex: 1, backgroundColor: colors.background },
  header: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'flex-start',
    paddingHorizontal: 16,
    paddingTop: 16,
    paddingBottom: 12,
  },
  headerTitle: { fontSize: 22, fontWeight: '800', color: colors.textPrimary },
  headerSub: { fontSize: 13, color: colors.textSecondary, marginTop: 2 },
  errorText: { color: colors.error, fontSize: 13, marginHorizontal: 16, marginBottom: 8 },
  ghostBtn: {
    paddingHorizontal: 12,
    paddingVertical: 7,
    borderRadius: 8,
    borderWidth: 1,
    borderColor: colors.border,
  },
  ghostBtnText: { fontSize: 13, color: colors.textSecondary },
  addBtn: {
    paddingHorizontal: 14,
    paddingVertical: 7,
    borderRadius: 8,
    backgroundColor: colors.primary,
  },
  addBtnText: { color: '#fff', fontSize: 13, fontWeight: '700' },
  scheduleCard: {
    backgroundColor: colors.surface,
    borderRadius: 14,
    padding: 16,
    marginBottom: 12,
    shadowColor: colors.dark,
    shadowOffset: { width: 0, height: 2 },
    shadowOpacity: 0.05,
    shadowRadius: 6,
    elevation: 2,
  },
  scheduleTop: { flexDirection: 'row', marginBottom: 12 },
  scheduleName: { fontSize: 15, fontWeight: '700', color: colors.textPrimary, marginBottom: 4 },
  scheduleTime: { fontSize: 13, color: colors.textSecondary, marginBottom: 2 },
  scheduleDays: { fontSize: 13, color: colors.textSecondary, marginBottom: 2 },
  scheduleAmount: { fontSize: 13, color: colors.textSecondary },
  statusBadge: { paddingHorizontal: 10, paddingVertical: 4, borderRadius: 20, height: 28, justifyContent: 'center' },
  statusActive: { backgroundColor: '#e6faf5' },
  statusPaused: { backgroundColor: '#f3f4f6' },
  statusText: { fontSize: 12, fontWeight: '600' },
  statusActiveText: { color: colors.success },
  statusPausedText: { color: colors.textSecondary },
  scheduleActions: { flexDirection: 'row', gap: 10 },
  editBtn: {
    flex: 1,
    paddingVertical: 8,
    borderRadius: 8,
    borderWidth: 1,
    borderColor: colors.border,
    alignItems: 'center',
  },
  editBtnText: { fontSize: 14, color: colors.textPrimary, fontWeight: '600' },
  deleteBtn: {
    flex: 1,
    paddingVertical: 8,
    borderRadius: 8,
    backgroundColor: '#fff0f3',
    alignItems: 'center',
  },
  deleteBtnText: { fontSize: 14, color: colors.error, fontWeight: '600' },
  emptyBox: { alignItems: 'center', paddingTop: 60 },
  emptyIcon: { fontSize: 48, marginBottom: 16 },
  emptyText: { fontSize: 16, color: colors.textPrimary, fontWeight: '600', marginBottom: 6 },
  emptyHint: { fontSize: 14, color: colors.textSecondary },
  // Modal
  modalHeader: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center', marginBottom: 20 },
  modalTitle: { fontSize: 20, fontWeight: '800', color: colors.textPrimary },
  fieldLabel: { fontSize: 14, fontWeight: '600', color: colors.textPrimary, marginBottom: 6, marginTop: 4 },
  input: {
    height: 48,
    borderWidth: 1.5,
    borderColor: colors.border,
    borderRadius: 10,
    paddingHorizontal: 14,
    fontSize: 15,
    color: colors.textPrimary,
    backgroundColor: colors.surface,
    marginBottom: 14,
  },
  chipGroup: { flexDirection: 'row', flexWrap: 'wrap', gap: 8, marginBottom: 16 },
  chip: {
    paddingHorizontal: 14,
    paddingVertical: 7,
    borderRadius: 20,
    borderWidth: 1.5,
    borderColor: colors.border,
    backgroundColor: colors.surface,
  },
  chipActive: { backgroundColor: colors.primary, borderColor: colors.primary },
  chipText: { fontSize: 13, color: colors.textSecondary, fontWeight: '600' },
  chipTextActive: { color: '#fff' },
  switchRow: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center', marginBottom: 24 },
  saveBtn: {
    height: 50,
    backgroundColor: colors.primary,
    borderRadius: 12,
    justifyContent: 'center',
    alignItems: 'center',
    marginBottom: 12,
  },
  saveBtnText: { color: '#fff', fontSize: 16, fontWeight: '700' },
  cancelBtn: {
    height: 50,
    borderRadius: 12,
    justifyContent: 'center',
    alignItems: 'center',
    borderWidth: 1.5,
    borderColor: colors.border,
  },
  cancelBtnText: { color: colors.textSecondary, fontSize: 16, fontWeight: '600' },
});
