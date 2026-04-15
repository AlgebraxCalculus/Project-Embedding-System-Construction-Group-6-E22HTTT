import { useState } from 'react';
import {
  ActivityIndicator,
  KeyboardAvoidingView,
  Platform,
  ScrollView,
  StyleSheet,
  Text,
  TextInput,
  TouchableOpacity,
  View,
} from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';
import { AuthAPI } from '../services/api';
import { colors } from '../theme/colors';

export default function RegisterScreen({ navigation }) {
  const [form, setForm] = useState({ username: '', password: '' });
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState('');
  const [success, setSuccess] = useState('');

  const handleSubmit = async () => {
    if (!form.username.trim() || !form.password.trim()) {
      setError('Vui lòng nhập đầy đủ thông tin');
      return;
    }
    setLoading(true);
    setError('');
    setSuccess('');
    try {
      await AuthAPI.register(form);
      setSuccess('Đăng ký thành công! Đang chuyển hướng...');
      setTimeout(() => navigation.navigate('Login'), 1500);
    } catch (err) {
      const apiError = err.response?.data;
      if (apiError?.message) setError(apiError.message);
      else if (Array.isArray(apiError?.errors) && apiError.errors.length > 0)
        setError(apiError.errors[0].msg);
      else setError('Đăng ký thất bại');
    } finally {
      setLoading(false);
    }
  };

  return (
    <SafeAreaView style={s.safe}>
      <KeyboardAvoidingView behavior={Platform.OS === 'ios' ? 'padding' : 'height'} style={{ flex: 1 }}>
        <ScrollView contentContainerStyle={s.scroll} keyboardShouldPersistTaps="handled">
          <View style={s.card}>
            <View style={s.logoBox}>
              <Text style={s.logoText}>SPF</Text>
            </View>
            <Text style={s.title}>Tạo tài khoản</Text>
            <Text style={s.subtitle}>Truy cập Smart Pet Feeder dashboard</Text>

            {error ? <Text style={s.errorText}>{error}</Text> : null}
            {success ? <Text style={s.successText}>{success}</Text> : null}

            <Text style={s.label}>Tên đăng nhập</Text>
            <TextInput
              style={s.input}
              placeholder="your_username"
              placeholderTextColor={colors.textSecondary}
              autoCapitalize="none"
              value={form.username}
              onChangeText={(v) => setForm((p) => ({ ...p, username: v }))}
            />

            <Text style={s.label}>Mật khẩu</Text>
            <TextInput
              style={s.input}
              placeholder="••••••••"
              placeholderTextColor={colors.textSecondary}
              secureTextEntry
              value={form.password}
              onChangeText={(v) => setForm((p) => ({ ...p, password: v }))}
            />

            <TouchableOpacity style={s.btnPrimary} onPress={handleSubmit} disabled={loading}>
              {loading ? (
                <ActivityIndicator color="#fff" />
              ) : (
                <Text style={s.btnPrimaryText}>Đăng ký</Text>
              )}
            </TouchableOpacity>

            <TouchableOpacity onPress={() => navigation.navigate('Login')}>
              <Text style={s.link}>
                Đã có tài khoản?{' '}
                <Text style={{ color: colors.primary }}>Đăng nhập</Text>
              </Text>
            </TouchableOpacity>
          </View>
        </ScrollView>
      </KeyboardAvoidingView>
    </SafeAreaView>
  );
}

const s = StyleSheet.create({
  safe: { flex: 1, backgroundColor: colors.dark },
  scroll: { flexGrow: 1, justifyContent: 'center', padding: 24 },
  card: {
    backgroundColor: colors.surface,
    borderRadius: 20,
    padding: 28,
    alignItems: 'center',
    shadowColor: colors.dark,
    shadowOffset: { width: 0, height: 8 },
    shadowOpacity: 0.15,
    shadowRadius: 20,
    elevation: 8,
  },
  logoBox: {
    width: 64,
    height: 64,
    borderRadius: 16,
    backgroundColor: colors.primary,
    justifyContent: 'center',
    alignItems: 'center',
    marginBottom: 16,
  },
  logoText: { color: '#fff', fontSize: 22, fontWeight: '800' },
  title: { fontSize: 22, fontWeight: '800', color: colors.textPrimary, marginBottom: 4 },
  subtitle: { fontSize: 14, color: colors.textSecondary, marginBottom: 24 },
  label: { alignSelf: 'flex-start', fontSize: 14, fontWeight: '600', color: colors.textPrimary, marginBottom: 6 },
  input: {
    width: '100%',
    height: 48,
    borderWidth: 1.5,
    borderColor: colors.border,
    borderRadius: 10,
    paddingHorizontal: 14,
    fontSize: 15,
    color: colors.textPrimary,
    backgroundColor: colors.background,
    marginBottom: 16,
  },
  btnPrimary: {
    width: '100%',
    height: 50,
    backgroundColor: colors.primary,
    borderRadius: 12,
    justifyContent: 'center',
    alignItems: 'center',
    marginTop: 8,
    marginBottom: 20,
  },
  btnPrimaryText: { color: '#fff', fontSize: 16, fontWeight: '700' },
  errorText: {
    color: colors.error,
    fontSize: 13,
    marginBottom: 12,
    alignSelf: 'flex-start',
    backgroundColor: '#fff0f3',
    padding: 10,
    borderRadius: 8,
    width: '100%',
  },
  successText: {
    color: colors.success,
    fontSize: 13,
    marginBottom: 12,
    alignSelf: 'flex-start',
    backgroundColor: '#f0fdf9',
    padding: 10,
    borderRadius: 8,
    width: '100%',
  },
  link: { fontSize: 14, color: colors.textSecondary },
});
