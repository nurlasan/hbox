/*
 * Тест кодировщиков (hbox-stf): живой звук с микрофона → MP3 (shine) и AAC (Espressif),
 * моно 44.1 кГц 128 кбит/с, AAC сразу в .m4a. Печатает загрузку CPU и выгружает записанное в консоль
 * для pcclient/bench.bat.
 */
#pragma once

void bench_run(void);
