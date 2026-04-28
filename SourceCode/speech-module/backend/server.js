const express = require('express');
const multer = require('multer');
const cors = require('cors');
const path = require('path');
const fs = require('fs');
const speechService = require('./speechService');

const app = express();

// Middleware
app.use(cors());
app.use(express.json());
app.use(express.static(path.join(__dirname, '../frontend')));

// Cấu hình multer để lưu file tạm
const upload = multer({
  dest: 'uploads/',
  limits: {
    fileSize: 10 * 1024 * 1024, // 10MB
  },
  fileFilter: (req, file, cb) => {
    // Chấp nhận các định dạng audio
    const allowedMimes = [
      'audio/webm',
      'audio/wav',
      'audio/mp3',
      'audio/mpeg',
      'audio/ogg',
      'audio/opus',
      'audio/aac',
      'audio/mp4',
      'audio/x-m4a',
      'audio/m4a',
      'video/mp4', // some Android encoders report M4A as video/mp4
    ];
    
    if (allowedMimes.includes(file.mimetype)) {
      cb(null, true);
    } else {
      cb(new Error('Invalid file type. Only audio files are allowed.'));
    }
  },
});

// Tạo thư mục uploads nếu chưa có
const uploadsDir = path.join(__dirname, 'uploads');
if (!fs.existsSync(uploadsDir)) {
  fs.mkdirSync(uploadsDir, { recursive: true });
}

// Route test API
app.get('/api/health', (req, res) => {
  res.json({
    status: 'ok',
    message: 'Speech-to-Text API is running!',
    timestamp: new Date().toISOString(),
  });
});

// Route chính: Speech-to-Text
app.post('/api/speech-to-text', upload.single('audio'), async (req, res) => {
  const startTime = Date.now();
  
  try {
    if (!req.file) {
      return res.status(400).json({
        error: 'No audio file provided',
        message: 'Please send an audio file in the request',
      });
    }

    console.log(`📥 Received audio file: ${req.file.originalname || req.file.filename}`);
    console.log(`📊 File size: ${(req.file.size / 1024).toFixed(2)} KB`);

    // Xử lý speech-to-text
    const result = await speechService.transcribeAudio(req.file.path, {
      languageCode: req.body.languageCode || 'vi-VN',
    });

    // Xóa file tạm sau khi xử lý
    fs.unlinkSync(req.file.path);

    const processingTime = ((Date.now() - startTime) / 1000).toFixed(2);

    console.log(`✅ Transcription: "${result.text}"`);
    console.log(`⏱️ Processing time: ${processingTime}s`);

    res.json({
      success: true,
      text: result.text,
      confidence: result.confidence,
      processingTime: `${processingTime}s`,
      language: result.languageCode,
    });

  } catch (error) {
    // Xóa file tạm nếu có lỗi
    if (req.file && fs.existsSync(req.file.path)) {
      fs.unlinkSync(req.file.path);
    }

    console.error('❌ Error:', error.message);
    
    res.status(500).json({
      success: false,
      error: error.message,
      details: process.env.NODE_ENV === 'development' ? error.stack : undefined,
    });
  }
});

// Route parse command từ text
app.post('/api/parse-command', (req, res) => {
  try {
    const { text } = req.body;

    if (!text || typeof text !== 'string') {
      return res.status(400).json({
        error: 'Text is required',
      });
    }

    const command = speechService.parseCommand(text);

    res.json({
      success: true,
      originalText: text,
      command: command,
    });

  } catch (error) {
    console.error('❌ Parse error:', error);
    res.status(500).json({
      success: false,
      error: error.message,
    });
  }
});

// Route kết hợp: Speech-to-Text + Parse Command
app.post('/api/speech-command', upload.single('audio'), async (req, res) => {
  const startTime = Date.now();

  try {
    if (!req.file) {
      return res.status(400).json({
        error: 'No audio file provided',
      });
    }

    // Bước 1: Speech-to-Text
    const transcription = await speechService.transcribeAudio(req.file.path, {
      languageCode: req.body.languageCode || 'vi-VN',
    });

    // Bước 2: Parse command
    const command = speechService.parseCommand(transcription.text);

    // Xóa file tạm
    fs.unlinkSync(req.file.path);

    const processingTime = ((Date.now() - startTime) / 1000).toFixed(2);

    res.json({
      success: true,
      transcription: transcription.text,
      confidence: transcription.confidence,
      command: command,
      processingTime: `${processingTime}s`,
    });

  } catch (error) {
    if (req.file && fs.existsSync(req.file.path)) {
      fs.unlinkSync(req.file.path);
    }

    console.error('❌ Error:', error);
    res.status(500).json({
      success: false,
      error: error.message,
    });
  }
});

// Error handling middleware
app.use((error, req, res, next) => {
  if (error instanceof multer.MulterError) {
    if (error.code === 'LIMIT_FILE_SIZE') {
      return res.status(400).json({
        error: 'File too large',
        message: 'Maximum file size is 10MB',
      });
    }
  }
  
  res.status(500).json({
    error: error.message || 'Internal server error',
  });
});

// 404 handler
app.use((req, res) => {
  res.status(404).json({
    error: 'Route not found',
  });
});

const PORT = process.env.PORT || 3001;

app.listen(PORT, () => {
  console.log('🚀 Speech-to-Text Server started!');
  console.log(`📡 Server running on http://localhost:${PORT}`);
  console.log(`🌐 Frontend: http://localhost:${PORT}`);
  console.log(`📋 API Health: http://localhost:${PORT}/api/health`);
  console.log('');
  console.log('📝 Available endpoints:');
  console.log('   POST /api/speech-to-text - Convert speech to text');
  console.log('   POST /api/parse-command - Parse text to command');
  console.log('   POST /api/speech-command - Speech to text + parse command');
  console.log('');
});

