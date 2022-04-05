import json
import subprocess
import time
import requests
import urllib.request
import zipfile
import srt
import datetime
import os

from datetime import datetime as dt
from vosk import KaldiRecognizer, Model
from pathlib import Path


WORDS_PER_LINE = 7
model_pre_path = 'https://alphacephei.com/vosk/models/'
model_list_url = model_pre_path + 'model-list.json'

class Transcriber:

    def transcribe(model, process, args):
        rec = KaldiRecognizer(model, 16000)
        rec.SetWords(True)
        tot_samples = 0
        result = list()
        subs = list()

        def get_result_and_tot_samples(rec, data, tot_samples, result):
            if rec.AcceptWaveform(data):
                tot_samples += len(data)
                result.append(json.loads(rec.Result()))
            return result, tot_samples

        while True:
            data = process.stdout.read(4000)
            if len(data) == 0:
                break
            if args.outputtype == 'txt':
                    result, tot_samples = get_result_and_tot_samples(rec, data, tot_samples, result)
            elif args.outputtype == 'srt':
                    result, tot_samples = get_result_and_tot_samples(rec, data, tot_samples, result)
        result.append(json.loads(rec.FinalResult()))
        if args.outputtype == 'srt':
            for i, res in enumerate(result):
                if not 'result' in res:
                    continue
                words = res['result']
                for j in range(0, len(words), WORDS_PER_LINE):
                    line = words[j : j + WORDS_PER_LINE]
                    s = srt.Subtitle(index=len(subs),
                            content = ' '.join([l['word'] for l in line]),
                            start=datetime.timedelta(seconds=line[0]['start']),
                            end=datetime.timedelta(seconds=line[-1]['end']))
                    subs.append(s)
        final_result = ''
        if args.outputtype == 'srt':
            final_result = srt.compose(subs)
        elif args.outputtype == 'txt':
            for i in range(len(result)):
                final_result += result[i]['text'] + ' '
        return final_result, tot_samples

    def resample_ffmpeg(infile):
        process = subprocess.Popen(
            ['ffmpeg', '-nostdin', '-loglevel', 'quiet', '-i', 
            infile, 
            '-ar', '16000','-ac', '1', '-f', 's16le', '-'], 
            stdout=subprocess.PIPE)
        return process

    def get_start_time():
        start_time = dt.now()
        return start_time

    def get_end_time(start_time):
        script_time = str(dt.now() - start_time)
        seconds = script_time[5:8].strip('0')
        mcseconds = script_time[8:].strip('0')
        return script_time.strip(':0'), seconds.rstrip('.'), mcseconds

    def get_file_list(args):
        files = os.listdir(args.input)
        arg_list = list()
        input_dir = args.input + '/'
        output_dir = args.output + '/'
        extension_i = f".{files[0].split('.')[1]}"
        format_o = f".{args.outputtype}"
        input_list = [input_dir + each for each in files]
        output_list = [output_dir + each.replace(extension_i, format_o) for each in files]
        [arg_list.extend([(input_list[i], output_list[i])]) for i in range(len(input_list))]
        return arg_list

    def get_list_models():
        response = requests.get(model_list_url)
        [print(response.json()[i]['name']) for i in range(len(response.json()))]
        exit(1)

    def get_model(args):
        if args.lang != 'en-us' or args.model_name != 'vosk-model-small-en-us-0.15':
            response = requests.get(model_list_url)
            for i in range(len(response.json())):
                if response.json()[i]['lang'] == args.lang and response.json()[i]['type'] == 'small' and response.json()[i]['obsolete'] == 'false' or response.json()[i]['name'] == args.model_name:
                    result_model = response.json()[i]['name']
        else:
            result_model = args.model_name
        model_path = Path.home() / '.cache' / 'vosk'
        if not Path.is_dir(model_path):
            Path.mkdir(model_path)
        model_location = Path(model_path, result_model)
        if not Path(model_location).exists():
            model_zip = str(model_path) + result_model + '.zip'
            model_url = model_pre_path + result_model + '.zip'
            urllib.request.urlretrieve(model_url, model_zip)
            with zipfile.ZipFile(model_path / model_zip, 'r') as model_ref:
                model_ref.extractall(model_path)
            Path.unlink(model_path / model_zip)
        model = Model(str(model_location))
        return model
