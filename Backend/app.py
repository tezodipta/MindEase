from flask import Flask, request, jsonify, send_file, Response
from google.cloud import speech_v1p1beta1 as speech
from google.cloud import texttospeech
from pyngrok import ngrok
from dotenv import load_dotenv
import os
import io
import json
import base64
import requests
import socket
from pathlib import Path

# Load environment variables
load_dotenv()
os.environ['GOOGLE_APPLICATION_CREDENTIALS'] = os.path.abspath(os.getenv('GOOGLE_APPLICATION_CREDENTIALS'))
ngrok_auth_token = os.getenv('NGROK_AUTH_TOKEN')


app = Flask(__name__)

ngrok.set_auth_token(ngrok_auth_token)  # Replace with your actual auth token

public_url = ngrok.connect(3000, hostname="hornet-upright-ewe.ngrok-free.app", bind_tls=False)
print(f"Public URL: {public_url}")

# Paths
record_file = Path('tmp/recording.wav')
voiced_file = Path('tmp/voicedby.wav')

# API Key
groq_api_key = os.getenv('GROQ_API_KEY')
should_download_file = False

# Google Cloud clients
speech_client = speech.SpeechClient()
tts_client = texttospeech.TextToSpeechClient()

# Upload Audio Endpoint
@app.route('/uploadAudio', methods=['POST'])
def upload_audio():
    global should_download_file
    should_download_file = False

    try:
        with open(record_file, 'wb') as f:
            data_size = 0
            while True:
                chunk = request.stream.read(4096)
                if not chunk:
                    break
                data_size += len(chunk)
                f.write(chunk)

        print(f"Audio upload complete. Total size: {data_size} bytes")
        transcription = speech_to_text_api()

        if transcription:
            print("Transcription successful, calling Groq...")
            call_groq(transcription)
            # call_custom_llm(transcription)  # Or replace with call_custom_llm(transcription)
            return transcription, 200
        else:
            return "Error transcribing audio", 200
    except Exception as e:
        print("Unexpected error:", e)
        return "Unexpected server error", 500

# Check readiness
@app.route('/checkVariable', methods=['GET'])
def check_variable():
    return jsonify({ "ready": should_download_file })

# Broadcast audio
@app.route('/broadcastAudio', methods=['GET'])
def broadcast_audio():
    if not voiced_file.exists():
        return '', 404
    return Response(
        open(voiced_file, 'rb'),
        mimetype='audio/wav',
        headers={"Content-Length": str(voiced_file.stat().st_size)}
    )

# Test audio file
@app.route('/test-audio', methods=['GET'])
def test_audio():
    return send_file(record_file)

# Test TTS output
@app.route('/test-response', methods=['GET'])
def test_response():
    return send_file(voiced_file)

# Status check
@app.route('/status', methods=['GET'])
def status():
    return jsonify({
        "recordingExists": record_file.exists(),
        "responseExists": voiced_file.exists(),
        "responseReady": should_download_file,
        "googleCredentials": bool(os.getenv('GOOGLE_APPLICATION_CREDENTIALS')),
        "groqKeyConfigured": bool(groq_api_key)
    })

# Speech to Text
def speech_to_text_api():
    try:
        if not record_file.exists():
            raise FileNotFoundError("Audio file not found")
        with open(record_file, "rb") as audio_file:
            content = audio_file.read()
        if not content:
            raise ValueError("Audio file is empty")

        audio = {"content": base64.b64encode(content).decode("utf-8")}
        config = {
            "encoding": "LINEAR16",
            "sample_rate_hertz": 16000,
            "language_code": "en-US"
        }

        response = speech_client.recognize(config=config, audio=audio)
        transcription = "\n".join(result.alternatives[0].transcript for result in response.results)
        print("Transcription:", transcription)
        return transcription

    except Exception as e:
        print("Error in speech_to_text_api:", e)
        return None

# Groq LLM Call
def call_groq(text):
    global should_download_file
    try:
        print("Sending to Groq:", text)
        api_url = "https://api.groq.com/openai/v1/chat/completions"
        headers = {
            "Content-Type": "application/json",
            "Authorization": f"Bearer {groq_api_key}"
        }

        payload = {
            "messages": [{"role": "user", "content": text}],
            "model": "llama-3.3-70b-versatile",
            "temperature": 1,
            "max_tokens": 150,
            "top_p": 1,
            "stream": False,
            "stop": None
        }

        response = requests.post(api_url, headers=headers, json=payload)
        groq_response = response.json()['choices'][0]['message']['content']
        print("Groq Response:", groq_response)
        gpt_response_to_speech(groq_response)

    except Exception as e:
        print("Error calling Groq API:", e)
        fallback = "I'm sorry, I couldn't process your request at this time."
        gpt_response_to_speech(fallback)

# Custom LLM Call (e.g., Google Colab)
def call_custom_llm(text):
    try:
        print("Sending to custom LLM:", text)
        response = requests.post(
            "https://nicely-funky-katydid.ngrok-free.app/generate",
            headers={"Content-Type": "application/json"},
            json={"prompt": text}
        )
        custom_response = response.json()
        print("Custom LLM Response:", custom_response)
        
        # Extract the actual response text from the dictionary
        response_text = custom_response.get('response', "I'm sorry, I couldn't process your request.")
        
        # Now pass the string, not the dictionary
        gpt_response_to_speech(response_text)
    except Exception as e:
        print("Error calling custom LLM API:", e)
        fallback = "I'm sorry, I couldn't process your request at this time."
        gpt_response_to_speech(fallback)

# GPT Response to TTS
def gpt_response_to_speech(gpt_response):
    global should_download_file
    try:
        if not gpt_response.strip():
            gpt_response = "I'm sorry, I don't have a response at this time."

        input_text = texttospeech.SynthesisInput(text=gpt_response)
        voice = texttospeech.VoiceSelectionParams(
            language_code="en-US", ssml_gender=texttospeech.SsmlVoiceGender.NEUTRAL
        )
        audio_config = texttospeech.AudioConfig(
            audio_encoding=texttospeech.AudioEncoding.LINEAR16,
            sample_rate_hertz=16000,
            # audio_channel_count=1,  # Set to mono
            effects_profile_id=["headphone-class-device"],
            pitch=0.0,
            speaking_rate=1.0,
        )

        response = tts_client.synthesize_speech(
            input=input_text, voice=voice, audio_config=audio_config
        )
        

        with open(voiced_file, "wb") as out:
            out.write(response.audio_content)

        print("Audio content written to file:", voiced_file)
        should_download_file = True
        print("TTS conversion complete, response ready for playback")

    except Exception as e:
        print("Error in TTS conversion:", e)
        should_download_file = False

# Server Start
if __name__ == '__main__':
    port = int(os.getenv('PORT', 3000))
    print(f"Server running at http://localhost:{port}/")

    # Show IP address
    hostname = socket.gethostname()
    local_ip = socket.gethostbyname(hostname)
    print(f"Server IP: {local_ip}")

    app.run(host='0.0.0.0', port=port)
