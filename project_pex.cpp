// project_pex.cpp : Defines the entry point for the console application.
//
#include <windows.h>

#include "stdafx.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <fstream>
#include <sstream>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libavutil/timestamp.h>
#include <libavutil/pixfmt.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include <opencv2/opencv.hpp>

typedef std::vector<uint8_t> bin_t;
typedef std::vector<std::vector<uint8_t>> rowbins_t;
typedef std::shared_ptr<rowbins_t> sprowbins_t;

/* Context structure, for better work */
struct SGridContext
{
	uint dimCols;
	uint dimRows;
	uint dimColsPx;
	uint dimColsPxLast;
	uint dimRowsPx;
	uint dimRowsPxLast;

	sprowbins_t spBins;
	sprowbins_t spLastRowBins;
};

/* Result writter for easy file handling */
class CResultWritter
{
private:
	std::string m_filePath;
	std::ofstream m_ofstream;

public:	
	
	bool m_disposed;

	CResultWritter()
	{
		m_disposed = true;
	}

	~CResultWritter()
	{
		if (!m_disposed) { closeFile(); }
	}

	void open(const std::string& filePath)
	{
		m_ofstream.open(filePath.c_str());
		m_disposed = false;
	}

	void addLine(const double& timeSec, const std::vector<uint8_t>& values)
	{
		m_ofstream << timeSec;
		for (const uint8_t &val : values)
		{
			m_ofstream << "," << static_cast<uint>(val);
		}

		m_ofstream << std::endl;
	}

	void closeFile()
	{
		m_ofstream.flush();
		m_ofstream.close();
		m_disposed = true;
	}
};

/* Exit program handling */
template <typename T>
struct scope_exit
{
	scope_exit(T&& t) : t_{ std::move(t) } {}
	~scope_exit() { t_(); }
	T t_;
};		

template <typename T>
scope_exit<T> make_scope_exit(T&& t) 
{
	return scope_exit<T> { std::move(t) };
}

/* Getting median */
template<typename T>
T getMedian(const std::vector<T>& vec)
{
	if (vec.size() == 0) { return 0; }
	else if (vec.size() % 2 == 0) { return (vec[vec.size() / 2 - 1] + vec[vec.size() / 2]) / 2; }
	else { return vec[vec.size() / 2];  }
};

/* Next three fncs are for parsing command arguments - very easily.
 * Probably, it's not absolutelly most robust way, but it's easy.
 */
bool cmdOptionExists(char** begin, char** end, const std::string& option)
{
	return std::find(begin, end, option) != end;
}

char* getCmdOption(char ** begin, char ** end, const std::string & option)
{
	char** itr = std::find(begin, end, option);
	if (itr != end && ++itr != end)
	{
		return *itr;
	}
	return 0;
}

int parseArg(const int argc,
	char** argv,
	std::string& inputFile,
	std::string& outputFile,
	uint& dimRows,
	uint& dimCols)
{
	char* value;
	std::stringstream ss;
	uint acc = 0;

	if (cmdOptionExists(argv, argv + argc, "-i"))
	{
		value = getCmdOption(argv, argv + argc, "-i");
		ss << value;
		ss >> inputFile;
		ss.clear();
		acc++;
	}

	if (cmdOptionExists(argv, argv + argc, "-o"))
	{
		value = getCmdOption(argv, argv + argc, "-o");
		ss << value;
		ss >> outputFile;
		ss.clear();
		acc++;
	}

	if (cmdOptionExists(argv, argv + argc, "-r"))
	{
		value = getCmdOption(argv, argv + argc, "-r");
		ss << value;
		ss >> dimRows;
		ss.clear();
		acc++;
	}

	if (cmdOptionExists(argv, argv + argc, "-c"))
	{
		value = getCmdOption(argv, argv + argc, "-c");
		ss << value;
		ss >> dimCols;
		ss.clear();
		acc++;
	}

	return acc >= 4 ?  0 : -1;
}

/* Open codecs */
int open_codec_context(int* stream_idx,
	AVCodecContext** dec_ctx, AVFormatContext* fmt_ctx, enum AVMediaType type,
	const std::string videoFilePath)
{
	int ret, stream_index;
	AVStream* st;
	AVCodec* dec = NULL;
	AVDictionary* opts = NULL;

	ret = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);
	if (ret < 0) {
		fprintf(stderr, "Could not find %s stream in input file '%s'\n",
			av_get_media_type_string(type), videoFilePath);
		return ret;
	}
	else {
		stream_index = ret;
		st = fmt_ctx->streams[stream_index];

		/* find decoder for the stream */
		dec = avcodec_find_decoder(st->codecpar->codec_id);
		if (!dec) {
			fprintf(stderr, "Failed to find %s codec\n",
				av_get_media_type_string(type));
			return AVERROR(EINVAL);
		}

		/* Allocate a codec context for the decoder */
		*dec_ctx = avcodec_alloc_context3(dec);
		if (!*dec_ctx) {
			fprintf(stderr, "Failed to allocate the %s codec context\n",
				av_get_media_type_string(type));
			return AVERROR(ENOMEM);
		}

		/* Copy codec parameters from input stream to output codec context */
		if ((ret = avcodec_parameters_to_context(*dec_ctx, st->codecpar)) < 0) {
			fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
				av_get_media_type_string(type));
			return ret;
		}

		/* Init the decoders, with or without reference counting */
		int refcount = 0;
		av_dict_set(&opts, "refcounted_frames", refcount ? "1" : "0", 0);
		if ((ret = avcodec_open2(*dec_ctx, dec, &opts)) < 0) {
			fprintf(stderr, "Failed to open %s codec\n",
				av_get_media_type_string(type));
			return ret;
		}
		*stream_idx = stream_index;
	}

	return 0;
}

/* Decodes ffmpeg's AVPacket to opencv's Mat */
int decodePacketToMat(std::shared_ptr<cv::Mat>& spMat, const AVPacket* pPacket,
	AVCodecContext* video_dec_ctx, AVFrame* frame)
{
	int ret = 0;
	int got_frame = 0;

	avcodec_send_packet(video_dec_ctx, pPacket);
	if (ret < 0)
		return 1;

	while (ret >= 0)
	{
		ret = avcodec_receive_frame(video_dec_ctx, frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			return 0;
		else if (ret < 0)
			return 3;

		int width = frame->width;
		int height = frame->height;

		if (spMat == nullptr)
			spMat = std::make_shared<cv::Mat>(height, width, CV_8UC1);

		int cvLinesizes[1];
		cvLinesizes[0] = spMat->step1();

		// Convert the colour format and write directly to the opencv matrix
		SwsContext* conversion = sws_getContext(width, height, (AVPixelFormat)frame->format, width, height, AV_PIX_FMT_GRAY8, SWS_FAST_BILINEAR, NULL, NULL, NULL);
		sws_scale(conversion, frame->data, frame->linesize, 0, height, &spMat->data, cvLinesizes);
		sws_freeContext(conversion);
	}

	return 0;
}

/* Creating function for intensities */
int createIntensityArray(const uint& dimCols,
                         const uint& dimRows,
                         std::shared_ptr<std::vector<uint8_t>>& spValues)
{
	uint numOfCells = dimCols * dimRows;

	spValues = std::make_shared<std::vector<uint8_t>>();
	spValues->reserve(numOfCells);
	spValues->insert(spValues->end(), numOfCells, 0);

	return 0;
}

/* Factory fnc for grid context */
int createGridContext(const uint& imgCols,
	                  const uint& imgRows,
	                  const uint& dimCols,
	                  const uint& dimRows,
                      SGridContext& context)
{
	context.dimCols = dimCols;
	context.dimRows = dimRows;
	context.dimColsPx = imgCols / dimCols;
	context.dimColsPxLast = imgCols % dimCols;
	context.dimRowsPx = imgRows / dimRows;
	context.dimRowsPxLast = imgRows % dimRows;

	bin_t bin; bin.insert(bin.end(), context.dimColsPx * context.dimRowsPx, 0);
	context.spBins = std::make_shared<rowbins_t>();
	context.spBins->reserve(dimCols);
	context.spBins->insert(context.spBins->end(), dimCols-1, bin);
	bin.resize(context.dimColsPxLast * context.dimRowsPx, 0);
	context.spBins->push_back(bin);

	if (context.dimRowsPxLast != 0)
	{
		bin.resize(context.dimColsPx * context.dimRowsPxLast, 0);
		context.spLastRowBins = std::make_shared<rowbins_t>();
		context.spLastRowBins->reserve(dimCols);
		context.spLastRowBins->insert(context.spBins->end(), dimCols - 1, bin);
		bin.resize(context.dimColsPxLast * context.dimRowsPxLast, 0);
		context.spLastRowBins->push_back(bin);
	}
	else { context.spLastRowBins = nullptr; }

	return 0;
}

/* Main fnc for split keyframe to grid  */
int splitImageByGrid(std::shared_ptr<cv::Mat>& spMat,
                     std::shared_ptr<std::vector<uint8_t>>& spValues,
	                 SGridContext& context)
{
	assert(spMat->depth() == CV_8U);

	int nRows = context.dimRows;
	int nCols = context.dimCols * spMat->channels();

	uint8_t median;
	uint iVal = 0;
	auto isContinuous = spMat->isContinuous();

	for (uint i = 0; i < spMat->rows; ++i)
	{
		auto iDimRow = i % context.dimRowsPx;
		auto iDim = i / context.dimRowsPx;
		sprowbins_t bins;
		uint lastDimPx = 0;

		uint8_t* p;
		if (isContinuous) { p = spMat->ptr<uint8_t>(0); }
		else { p = spMat->ptr<uint8_t>(i); }

		// Note: this last step should be proceed as "last algorithm step"
		// -> would save some dynamic decisions -> better performance
		if ((iDim < context.dimRows - 1) || (context.dimRowsPxLast == 0))
		{
			bins = context.spBins;
			lastDimPx = context.dimRowsPx;
		}
		else
		{
			bins = context.spLastRowBins;
			lastDimPx = context.dimRowsPxLast;
		}

		uint j = 0;
		for (; j < context.dimCols - 1; j++)
		{
			// Note: probably this dynamic decisions would be better
			// to split to separater fncs -> performance reasons
			if (isContinuous)
			{
				memcpy((*bins)[j].data() + (context.dimColsPx * iDimRow),
					p + (i * spMat->cols) + (j * context.dimColsPx),
					context.dimColsPx);
			}
			else
			{
				memcpy((*bins)[j].data() + (context.dimColsPx * iDimRow),
					p + (j * context.dimColsPx),
					context.dimColsPx);
			}
		}

		// last bin
		if (isContinuous)
		{
			memcpy((*bins)[j].data() + (context.dimColsPxLast * iDimRow),
				p + (i * spMat->cols) + (j * context.dimColsPxLast),
				context.dimColsPxLast);
		}
		else
		{
			memcpy((*bins)[j].data() + (context.dimColsPxLast * iDimRow),
				p + (j * context.dimColsPxLast),
				context.dimColsPxLast);
		}

		// last row for bins -> save medians
		if (iDimRow == lastDimPx - 1)
		{
			for (rowbins_t::iterator it = bins->begin(); it != bins->end(); it++)
			{
				std::sort(it->begin(), it->end());
				median = getMedian<uint8_t>(*it);
				(*spValues)[iVal] = median;
				iVal++;
			}
		}
	}

	return 0;
}

int main(int argc, char* argv[])
{
	AVFormatContext* fmt_ctx = NULL;
	int video_stream_idx = -1;
	AVCodecContext* video_dec_ctx = NULL;
	AVStream* video_stream = NULL;
	AVPacket pkt;
	AVCodecContext* audio_dec_ctx = NULL;
	AVFrame *frame = NULL;

	std::string inputFile, outputFile;
	uint dimCols, dimRows;

	// create writer
	CResultWritter writter;

	auto cleanupRoutine = make_scope_exit([fmt_ctx, video_dec_ctx, frame, &writter]() mutable
	{

		avcodec_free_context(&video_dec_ctx);
		avformat_close_input(&fmt_ctx);
		av_frame_free(&frame);
		writter.closeFile();
	});

	if (parseArg(argc, argv, inputFile, outputFile, dimRows, dimCols) != 0)
	{
		std::cerr << "Error through parsing arguments! Some is missing or wrong." << std::endl;
	}
	writter.open(outputFile);

	// open input file, and allocate format context
	if (avformat_open_input(&fmt_ctx, inputFile.c_str(), NULL, NULL) < 0)
	{
		std::cerr << "Could not open source file '" << inputFile << "' !";
		exit(1);
	}

	/* retrieve stream information */
	if (avformat_find_stream_info(fmt_ctx, NULL) < 0)
	{
		std::cerr << "Could not find stream information!\n" << std::endl;
		exit(2);
	}

	if (open_codec_context(&video_stream_idx, &video_dec_ctx, fmt_ctx, AVMEDIA_TYPE_VIDEO, inputFile) >= 0)
	{
		video_stream = fmt_ctx->streams[video_stream_idx];
	}
	else
	{
		std::cerr << "Could open proper video codec!\n" << std::endl;
		exit(3);
	}
	
	frame = av_frame_alloc();
	if (!frame)
	{
		std::cerr << "Could not allocate frame\n" << std::endl;
		exit(4);
	}

	/* initialize packet, set data to NULL, let the demuxer fill it */
	av_init_packet(&pkt);
	pkt.data = NULL;
	pkt.size = 0;
	int keyframe = -1;
	uint32_t frameCnt = 0;
	std::shared_ptr<cv::Mat> spMat;
	std::shared_ptr<std::vector<uint8_t>> spValues;
	SGridContext gridContext;

	createIntensityArray(32,
						 32,
						 spValues);

	createGridContext(fmt_ctx->streams[video_stream_idx]->codecpar->width,
		              fmt_ctx->streams[video_stream_idx]->codecpar->height,
		              32,
		              32,
	                  gridContext);
	
	// av_read_frame() is through whole video is absolutely bad!
	// Unfortunately, I wasn't able to use av_seek_frame in right way - it
	// didn't work for my expectations. For sure, key frames should be find
	// directly by seeking - it should be quite effective for reading engine.
	while (av_read_frame(fmt_ctx, &pkt) >= 0)
	{
		if ((pkt.stream_index == video_stream_idx) && (pkt.flags & AV_PKT_FLAG_KEY))
		{
			std::cout << "-----> Process keyframe #" << frameCnt++ << " <-----" << std::endl;
			std::cout << "Decode and load to Mat" << std::endl;
			if (decodePacketToMat(spMat, &pkt, video_dec_ctx, frame) != 0)
			{
				std::cerr << "Decode error!\n" << std::endl;
				exit(5);
			}

			splitImageByGrid(spMat, spValues, gridContext);

			double time = pkt.pts * static_cast<double>(fmt_ctx->streams[video_stream_idx]->time_base.num)
					/ fmt_ctx->streams[video_stream_idx]->time_base.den;
			writter.addLine(time, *spValues);
		}
	}

	writter.closeFile();
	cv::waitKey(0);
	return 0;
}