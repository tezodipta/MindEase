const express = require('express');
const cors = require('cors');
const fs = require('fs');
const path = require('path');
const speech = require('@google-cloud/speech');
const textToSpeech = require('@google-cloud/text-to-speech');
const axios = require('axios');
require('dotenv').config();
require('express-async-errors');

const port = process.env.PORT || 3000;
process.env.GOOGLE_APPLICATION_CREDENTIALS = path.resolve(process.env.GOOGLE_APPLICATION_CREDENTIALS);



// Init express
const app = express();


// Path to files for production
const recordFile = path.join(__dirname, 'tmp', 'recording.wav');
const voicedFile = path.join(__dirname, 'tmp', 'voicedby.wav');

// API Key
const groqApiKey = process.env.GROQ_API_KEY;
let shouldDownloadFile = false;

// Init Google Cloud clients
const speechClient = new speech.SpeechClient();
const ttsClient = new textToSpeech.TextToSpeechClient();

// Middleware for data processing
app.use(express.urlencoded({ extended: true }));
app.use(express.json());

// Upload Audio
app.post('/uploadAudio', async (req, res) => {
  try {
    shouldDownloadFile = false;
    const recordingFile = fs.createWriteStream(recordFile);
    let dataSize = 0;

    req.on('data', (chunk) => {
      dataSize += chunk.length;
    });

    req.pipe(recordingFile);

    req.on('end', async () => {
      recordingFile.end();
      console.log(`Audio upload complete. Total size: ${dataSize} bytes`);

      try {
        const transcription = await speechToTextAPI();
        if (transcription) {
          console.log("Transcription successful, calling Groq...");
          res.status(200).send(transcription);
          //calling the groq api 
          callGroq(transcription);
          // Uncomment the line below to call the custom LLM API instead of Groq
          // callCustomLLM(transcription);
        } else {
          console.error("Transcription failed, sending error response");
          res.status(200).send('Error transcribing audio');
        }
      } catch (err) {
        console.error('Error in audio processing:', err);
        res.status(200).send('Error processing audio');
      }
    });

    req.on('error', (err) => {
      console.error('Error writing file:', err);
      res.status(500).send('Error uploading audio');
    });
  } catch (error) {
    console.error('Unexpected error:', error);
    res.status(500).send('Unexpected server error');
  }
});

// Check Variable
app.get('/checkVariable', (req, res) => {
  res.json({ ready: shouldDownloadFile });
});

// Broadcast Audio
app.get('/broadcastAudio', (req, res) => {
  fs.stat(voicedFile, (err, stats) => {
    if (err) return res.sendStatus(404);

    res.writeHead(200, { 'Content-Type': 'audio/wav', 'Content-Length': stats.size });
    const readStream = fs.createReadStream(voicedFile);
    readStream.pipe(res);
  });
});

// Test endpoints
app.get('/test-audio', (req, res) => {
  res.sendFile(recordFile);
});

app.get('/test-response', (req, res) => {
  res.sendFile(voicedFile);
});

app.get('/status', (req, res) => {
  res.json({
    recordingExists: fs.existsSync(recordFile),
    responseExists: fs.existsSync(voicedFile),
    responseReady: shouldDownloadFile,
    googleCredentials: !!process.env.GOOGLE_APPLICATION_CREDENTIALS,
    groqKeyConfigured: !!groqApiKey
  });
});

// Start Server
app.listen(port, () => {
  console.log(`Server running at http://localhost:${port}/`);
  //printing the ip 
  const interfaces = require('os').networkInterfaces();
  for (const interfaceName in interfaces) {
    const addresses = interfaces[interfaceName];
    for (const address of addresses) {
      if (address.family === 'IPv4' && !address.internal) {
        console.log(`Server IP: ${address.address}`);
      }
    }
  }
});

// Speech to Text using Google
async function speechToTextAPI() {
  try {
    // Check if file exists
    if (!fs.existsSync(recordFile)) {
      throw new Error('Audio file not found');
    }

    // Read audio file and convert to base64
    const fileContent = fs.readFileSync(recordFile);
    console.log('Audio File Size:', fileContent.length);

    if (!fileContent || fileContent.length === 0) {
      throw new Error('Audio file is empty');
    }

    const audio = {
      content: fileContent.toString('base64'),
    };

    const config = {
      encoding: 'LINEAR16',
      sampleRateHertz: 16000,  // Match ESP32 recording rate
      languageCode: 'en-US',
    };

    const request = {
      audio: audio,
      config: config,
    };

    // Call Google Speech-to-Text API
    const [response] = await speechClient.recognize(request);

    const transcription = response.results.map(result => result.alternatives[0].transcript).join('\n');
    console.log('Transcription:', transcription);
    return transcription;
  } catch (error) {
    console.error('Error in speechToTextAPI:', error);
    return null;
  }
}

// Call Groq API
async function callGroq(text) {
  try {
    console.log('Sending to Groq:', text);

    const apiUrl = "https://api.groq.com/openai/v1/chat/completions";

    const response = await axios.post(
      apiUrl,
      {
        messages: [
          {
            role: "user",
            content: text
          }
        ],
        model: "llama-3.3-70b-versatile",
        temperature: 1,
        max_tokens: 150,
        top_p: 1,
        stream: false,
        stop: null
      },
      {
        headers: {
          'Content-Type': 'application/json',
          'Authorization': `Bearer ${process.env.GROQ_API_KEY}`
        }
      }
    );

    // Extract the AI response
    const groqResponse = response.data.choices[0].message.content;
    console.log('Groq Response:', groqResponse);

    // Convert response to speech
    await GptResponsetoSpeech(groqResponse);

  } catch (error) {
    console.error('Error calling Groq API:');
    if (error.response) {
      console.error('Response status:', error.response.status);
      console.error('Response data:', error.response.data);
    } else {
      console.error(error.message);
    }

    // Send a fallback response in case of error
    const fallbackResponse = "I'm sorry, I couldn't process your request at this time.";
    await GptResponsetoSpeech(fallbackResponse);
  }
}

//calling custom llm api which is in google colab 
// Call custom local LLM API instead of Groq
async function callCustomLLM(text) {
  try {
    console.log('Sending to custom LLM:', text);

    const apiUrl = "https://nicely-funky-katydid.ngrok-free.app/generate";

    const response = await axios.post(
      apiUrl,
      { prompt: text },
      {
        headers: {
          'Content-Type': 'application/json'
        }
      }
    );

    // Extract the string response from the object
    const customResponse = response.data.response; // Access the 'response' property
    console.log('Custom LLM Response:', response.data);

    // Convert response to speech
    await GptResponsetoSpeech(customResponse);
  } catch (error) {
    console.error('Error calling custom LLM API:', error.message);
    const fallbackResponse = "I'm sorry, I couldn't process your request at this time.";
    await GptResponsetoSpeech(fallbackResponse);
  }
}


// Text to Speech using Google
// In the server.js file:
async function GptResponsetoSpeech(gptResponse) {
  try {
    // Ensure we have a valid response
    if (!gptResponse || gptResponse.trim() === '') {
      gptResponse = "I'm sorry, I don't have a response at this time.";
    }

    const request = {
      input: { text: gptResponse },
      voice: { languageCode: 'en-US', ssmlGender: 'NEUTRAL' },
      audioConfig: {
        audioEncoding: 'LINEAR16',
        sampleRateHertz: 16000,  // Make sure this matches ESP32
        effectsProfileId: ['headphone-class-device'],
        pitch: 0.0,
        speakingRate: 1.0,
        audioChannelCount: 1
      },
    };

    const [response] = await ttsClient.synthesizeSpeech(request);
    fs.writeFileSync(voicedFile, response.audioContent, 'binary');
    shouldDownloadFile = true;
    console.log("TTS conversion complete, response ready for playback");
  } catch (error) {
    console.error('Error in Text-to-Speech conversion:', error);
    shouldDownloadFile = false;
  }
}