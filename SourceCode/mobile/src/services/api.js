import axios from 'axios';
import AsyncStorage from '@react-native-async-storage/async-storage';

const BASE_URL = process.env.EXPO_PUBLIC_API_BASE_URL || 'http://localhost:5000';
const SPEECH_URL = process.env.EXPO_PUBLIC_SPEECH_URL || BASE_URL.replace(':5000', ':3001');

const api = axios.create({ baseURL: BASE_URL });

// Gắn JWT vào mọi request (thay localStorage → AsyncStorage)
api.interceptors.request.use(async (config) => {
  const token = await AsyncStorage.getItem('token');
  if (token) {
    config.headers.Authorization = `Bearer ${token}`;
  }
  return config;
});

export const AuthAPI = {
  login: ({ username, password }) => api.post('/api/auth/login', { username, password }),
  register: ({ username, password }) => api.post('/api/auth/register', { username, password }),
};

export const ScheduleAPI = {
  list: () => api.get('/api/schedules/get'),
  create: (payload) => api.post('/api/schedules/create', payload),
  update: (id, payload) => api.put(`/api/schedules/${id}`, payload),
  remove: (id) => api.delete(`/api/schedules/${id}`),
};

export const FeedAPI = {
  manual: () => api.post('/api/feed/manual'),
  voice: (voiceCommand) => api.post('/api/feed/voice', { text: voiceCommand }),
  weeklyStats: () => api.get('/api/feed/stats/weekly'),
  history: (limit = 20) => api.get('/api/feed/history', { params: { limit } }),
};

export const SpeechAPI = {
  transcribe: (fileUri) => {
    const formData = new FormData();
    formData.append('audio', {
      uri: fileUri,
      type: 'audio/wav',
      name: 'recording.wav',
    });
    formData.append('languageCode', 'vi-VN');
    return axios.post(`${SPEECH_URL}/api/speech-to-text`, formData, {
      headers: { 'Content-Type': 'multipart/form-data' },
      timeout: 30000,
    });
  },
};

export default api;
