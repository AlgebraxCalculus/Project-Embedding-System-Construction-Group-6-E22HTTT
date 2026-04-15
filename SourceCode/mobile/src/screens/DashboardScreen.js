import { useEffect, useMemo, useState } from 'react';
import {
  ActivityIndicator,
  Dimensions,
  FlatList,
  RefreshControl,
  ScrollView,
  StyleSheet,
  Text,
  TouchableOpacity,
  View,
} from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';
import { LineChart } from 'react-native-chart-kit';
import { FeedAPI } from '../services/api';
import { useAuth } from '../context/AuthContext';
import { colors } from '../theme/colors';

const screenWidth = Dimensions.get('window').width;

export default function DashboardScreen() {
  const { user, logout } = useAuth();
  const [weeklyStats, setWeeklyStats] = useState([]);
  const [loading, setLoading] = useState(false);
  const [refreshing, setRefreshing] = useState(false);
  const [error, setError] = useState('');

  const fetchStats = async () => {
    setLoading(true);
    setError('');
    try {
      const { data } = await FeedAPI.weeklyStats();
      setWeeklyStats(data.data || []);
    } catch (err) {
      setError(err.response?.data?.message || 'Không thể tải thống kê');
    } finally {
      setLoading(false);
    }
  };

  const onRefresh = async () => {
    setRefreshing(true);
    await fetchStats();
    setRefreshing(false);
  };

  useEffect(() => { fetchStats(); }, []);

  const chartData = useMemo(() => {
    if (!weeklyStats.length) return null;
    const labels = weeklyStats.map((s) => {
      const d = new Date(s.date);
      return `${d.getMonth() + 1}/${d.getDate()}`;
    });
    const data = weeklyStats.map((s) => s.totalAmount || 0);
    return { labels, datasets: [{ data }] };
  }, [weeklyStats]);

  return (
    <SafeAreaView style={s.safe}>
      <ScrollView
        style={s.scroll}
        refreshControl={<RefreshControl refreshing={refreshing} onRefresh={onRefresh} tintColor={colors.primary} />}
      >
        {/* Header */}
        <View style={s.header}>
          <View>
            <Text style={s.headerTitle}>Device Dashboard</Text>
            <Text style={s.headerSub}>Thống kê cho ăn</Text>
          </View>
          <View style={s.headerRight}>
            <Text style={s.userChip}>👤 {user?.username}</Text>
            <TouchableOpacity style={s.logoutBtn} onPress={logout}>
              <Text style={s.logoutText}>Đăng xuất</Text>
            </TouchableOpacity>
          </View>
        </View>

        {/* Chart Card */}
        <View style={s.card}>
          <View style={s.cardHeader}>
            <Text style={s.cardTitle}>Lượng thức ăn (7 ngày)</Text>
            <TouchableOpacity onPress={fetchStats} disabled={loading}>
              <Text style={s.refreshBtn}>Làm mới</Text>
            </TouchableOpacity>
          </View>

          {error ? <Text style={s.errorText}>{error}</Text> : null}
          {loading && !refreshing ? <ActivityIndicator color={colors.primary} style={{ marginVertical: 20 }} /> : null}

          {chartData ? (
            <LineChart
              data={chartData}
              width={screenWidth - 48}
              height={220}
              yAxisSuffix=" g"
              chartConfig={{
                backgroundColor: colors.surface,
                backgroundGradientFrom: colors.surface,
                backgroundGradientTo: colors.surface,
                decimalPlaces: 0,
                color: (opacity = 1) => `rgba(31, 177, 255, ${opacity})`,
                labelColor: () => colors.textSecondary,
                style: { borderRadius: 12 },
                propsForDots: { r: '5', strokeWidth: '2', stroke: colors.primary },
              }}
              bezier
              style={{ borderRadius: 12, marginTop: 8 }}
            />
          ) : (
            !loading && (
              <Text style={s.emptyText}>Chưa có dữ liệu. Hãy cho thú cưng ăn trước!</Text>
            )
          )}
        </View>

        {/* Table Card */}
        <View style={s.card}>
          <Text style={s.cardTitle}>Tóm tắt tuần</Text>
          <Text style={s.cardSub}>Thống kê 7 ngày qua</Text>

          {/* Table Header */}
          <View style={[s.row, s.tableHead]}>
            <Text style={[s.cell, s.headCell]}>Ngày</Text>
            <Text style={[s.cell, s.headCell]}>Tổng lượng</Text>
            <Text style={[s.cell, s.headCell]}>Số lần</Text>
          </View>

          {weeklyStats.length > 0 ? (
            weeklyStats.map((stat, idx) => {
              const d = new Date(stat.date);
              const label = d.toLocaleDateString('vi-VN', { month: 'short', day: 'numeric', year: 'numeric' });
              return (
                <View key={stat.date || idx} style={[s.row, idx % 2 === 1 && s.rowAlt]}>
                  <Text style={s.cell}>{label}</Text>
                  <Text style={s.cell}>{stat.totalAmount || 0} g</Text>
                  <Text style={s.cell}>{stat.feedCount || 0} lần</Text>
                </View>
              );
            })
          ) : (
            <Text style={s.emptyText}>Không có dữ liệu</Text>
          )}
        </View>

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
    paddingBottom: 12,
  },
  headerTitle: { fontSize: 22, fontWeight: '800', color: colors.textPrimary },
  headerSub: { fontSize: 13, color: colors.textSecondary, marginTop: 2 },
  headerRight: { alignItems: 'flex-end', gap: 6 },
  userChip: { fontSize: 13, color: colors.textSecondary },
  logoutBtn: {
    paddingHorizontal: 12,
    paddingVertical: 5,
    borderRadius: 8,
    borderWidth: 1,
    borderColor: colors.border,
  },
  logoutText: { fontSize: 13, color: colors.textSecondary },
  card: {
    backgroundColor: colors.surface,
    borderRadius: 16,
    marginHorizontal: 16,
    marginBottom: 16,
    padding: 16,
    shadowColor: colors.dark,
    shadowOffset: { width: 0, height: 2 },
    shadowOpacity: 0.06,
    shadowRadius: 8,
    elevation: 3,
  },
  cardHeader: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center', marginBottom: 4 },
  cardTitle: { fontSize: 16, fontWeight: '700', color: colors.textPrimary },
  cardSub: { fontSize: 13, color: colors.textSecondary, marginBottom: 12 },
  refreshBtn: { fontSize: 13, color: colors.primary, fontWeight: '600' },
  errorText: { color: colors.error, fontSize: 13, marginVertical: 8 },
  emptyText: { color: colors.textSecondary, fontSize: 14, textAlign: 'center', paddingVertical: 20 },
  row: { flexDirection: 'row', paddingVertical: 10, borderBottomWidth: 1, borderBottomColor: colors.border },
  rowAlt: { backgroundColor: '#f8fbff' },
  tableHead: { backgroundColor: colors.background, borderRadius: 8, marginBottom: 2 },
  cell: { flex: 1, fontSize: 13, color: colors.textPrimary, textAlign: 'center' },
  headCell: { fontWeight: '700', color: colors.textSecondary, fontSize: 12 },
});
