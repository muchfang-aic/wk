#!/usr/bin/env python3

import logging
import argparse

from datetime import datetime as dt
from transcriber import Transcriber
from multiprocessing.dummy import Pool
from pathlib import Path


parser = argparse.ArgumentParser(
        description = 'The program transcripts audiofile and displays result in selected format')
parser.add_argument(
        '-model', type=str,
        help='model path')
parser.add_argument(
        '-models_list', default=False, action='store_true', 
        help='list of all available models')
parser.add_argument(
        '-languages_list', default=False, action='store_true',
        help='list of all available languages')
parser.add_argument(
        '-model_name',  default='vosk-model-small-en-us-0.15', type=str,
        help='select model current language type')
parser.add_argument(
        '-lang', type=str,
        help='smallest available model for selected language')
parser.add_argument(
        '-input', type=str,
        help='audiofile')
parser.add_argument(
        '-output', default='', type=str,
        help='optional output filename path')
parser.add_argument(
        '-otype', '--outputtype', default='txt', type=str,
        help='optional arg output data type')
parser.add_argument(
        '--log', default='INFO',
        help='logging level')

args = parser.parse_args()
log_level = args.log.upper()
logging.getLogger().setLevel(log_level)
logging.info('checking args')

def get_results(inputdata):
    logging.info('converting audiofile to 16K sampled wav')
    stream = transcriber.resample_ffmpeg(inputdata[0])
    logging.info('complete')
    logging.info('starting transcription')
    final_result, tot_samples = transcriber.transcribe(model, stream, args)
    logging.info('complete')
    if args.output:
        with open(inputdata[1], 'w', encoding='utf-8') as fh:
            fh.write(final_result)
        logging.info('output written to %s' % (inputdata[1]))
    else:
        print(final_result)
    return final_result, tot_samples

def main(args):
    global model
    global transcriber
    transcriber = Transcriber()
    transcriber.check_args(args)
    if args.input:
        model = transcriber.get_model(args)
        if Path(args.input).is_dir() and Path(args.output).is_dir():
            file_list = transcriber.get_file_list(args)
            with Pool() as pool:
                for final_result, tot_samples in pool.map(get_results, file_list):
                    return final_result, tot_samples
        elif Path(args.input).is_file():
            inputdata = (args.input, args.output)
            final_result, tot_samples = get_results(inputdata)
        elif not Path(args.input).exists() or not Path(args.output).exists():
            logging.info('Please set correct input/output paths')
            exit(1)
        return final_result, tot_samples
    else:
        logging.info('Please set input argument')
        exit(1)

if __name__ == '__main__':

    def get_start_time():
        start_time = dt.now()
        return start_time

    def get_end_time(start_time):
        script_time = str(dt.now() - start_time)
        seconds = script_time[5:8].strip('0')
        mcseconds = script_time[8:].strip('0')
        return script_time.strip(':0'), seconds.rstrip('.'), mcseconds

    start_time = get_start_time()
    tot_samples = main(args)[1]
    diff_end_start, sec, mcsec = get_end_time(start_time)
    logging.info(f'''Execution time: {sec} sec, {mcsec} mcsec; xRT: {format(tot_samples / 16000.0 / float(diff_end_start), '.3f')}''')
