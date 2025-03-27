from fastapi import FastAPI
from pydantic import BaseModel
import torch
import gc
from transformers import AutoTokenizer, AutoModelForCausalLM ,BitsAndBytesConfig

# Load model once during startup
model_path = "./optimized_model/fp16_model"
quantization_config = BitsAndBytesConfig(load_in_4bit=True)
# Load model in 4-bit mode
model = AutoModelForCausalLM.from_pretrained(
    model_path,
    quantization_config=quantization_config,
    device_map="auto",
    local_files_only=True
)
tokenizer = AutoTokenizer.from_pretrained(model_path)
from accelerate import infer_auto_device_map

device_map = infer_auto_device_map(model, max_memory={"cuda": "4GB", "cpu": "12GB"})
model = model.to(device_map)

model.eval()

# Enable optimizations for faster inference
if torch.cuda.is_available():
    torch.backends.cuda.matmul.allow_tf32 = True  # Use TF32 for faster matrix ops

# Compile model for improved speed (if using PyTorch 2.0+)
if hasattr(torch, "compile"):
    model = torch.compile(model)

# Initialize FastAPI
app = FastAPI()

class RequestBody(BaseModel):
    user_input: str
    max_new_tokens: int = 100

@app.post("/generate")
async def generate_text(request: RequestBody):
    """API endpoint to generate AI responses."""
    inputs = tokenizer(
        f"User: {request.user_input}\nAI:", 
        return_tensors="pt", 
        padding=True, 
        truncation=True,
        max_length=512  # Adjust if necessary
    )
    
    # Explicitly set attention mask to avoid warnings
    inputs = {key: value.to(model.device) for key, value in inputs.items()}
    
    with torch.no_grad():
        outputs = model.generate(
            inputs["input_ids"],
            attention_mask=inputs["attention_mask"],  # Fixes attention mask warning
            max_new_tokens=request.max_new_tokens,
            temperature=0.7,
            top_p=0.9,
            do_sample=True,
            pad_token_id=tokenizer.pad_token_id or tokenizer.eos_token_id,  # Ensure pad token is properly set
            eos_token_id=tokenizer.eos_token_id,
        )

    response = tokenizer.decode(outputs[0], skip_special_tokens=True).replace(
        f"User: {request.user_input}\nAI:", "").strip()

    # Clean up GPU memory only when necessary
    if torch.cuda.is_available():
        torch.cuda.empty_cache()

    return {"response": response}

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000)
