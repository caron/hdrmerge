#include "hdrmerge.h"

#include <mutex>
#include <unistd.h>
#include <boost/format.hpp>

#include <exiv2/image.hpp>
#include <exiv2/easyaccess.hpp>

#include "rawspeed/RawSpeed/StdAfx.h"
#include "rawspeed/RawSpeed/FileReader.h"
#include "rawspeed/RawSpeed/RawDecoder.h"
#include "rawspeed/RawSpeed/RawParser.h"
#include "rawspeed/RawSpeed/CameraMetaData.h"
#include "rawspeed/RawSpeed/ColorFilterArray.h"

using namespace RawSpeed;

static std::unique_ptr<CameraMetaData> __metadata;
static std::mutex __metadata_mutex;

/**
 * Find all images in an exposure series and check that some sensible
 * base requirements are satisfied, i.e.
 *  - all images use the same ISO speed and aperture setting
 *  - the images were taken using manual focus and manual exposure mode
 *  - there are no duplicate exposures.
 */
void ExposureSeries::add(const char *fmt) {
	bool success = false;

	for (int exposure = 0; ; ++exposure) {
		char filename[1024];
		snprintf(filename, sizeof(filename), fmt, exposure);
		Exposure exp(filename);

		if (access(filename, F_OK) != 0)
			break;

		if (exposure == 1 && strchr(fmt, '%') == NULL)
			break; /* Just one image -- stop */

		success = true;
		exposures.push_back(exp);
	}

	if (!success) {
		/* Maybe the sequence starts at 1? */
		for (int exposure = 1; ; ++exposure) {
			char filename[1024];
			snprintf(filename, sizeof(filename), fmt, exposure);
			Exposure exp(filename);

			if (access(filename, F_OK) != 0)
				break;

			exposures.push_back(exp);
		}
	}
}

void ExposureSeries::check() {
	float isoSpeed = -1, aperture = -1;

	for (size_t exposure=0; exposure<exposures.size(); ++exposure) {
		Exposure &exp = exposures[exposure];

		Exiv2::Image::AutoPtr image = Exiv2::ImageFactory::open(exp.filename);
		if (image.get() == 0)
			throw std::runtime_error("\"" + exp.filename + "\": could not open RAW file!");
		image->readMetadata();

		Exiv2::ExifData &exifData = image->exifData();

		Exiv2::ExifData::const_iterator it;
		for (it = exifData.begin(); it != exifData.end(); ++it) {
			if (it->value().size() > 100) /* Reject huge attributes */
				continue;
			/* Collect the remainder */
			std::string value = it->toString();
			if (metadata.find(it->key()) != metadata.end()) {
				std::string current = metadata[it->key()];
				if (value == current)
					continue;
				metadata[it->key()] = current + std::string("; ") + value;
			} else {
				metadata[it->key()] = value;
			}
		}

		it = exifData.findKey(Exiv2::ExifKey("Exif.Photo.ShutterSpeedValue"));
		if (it == exifData.end())
			throw std::runtime_error("\"" + exp.filename + "\": could not extract the exposure time!");
		exp.exposure = std::pow(2, -it->toFloat());

		it = Exiv2::exposureTime(exifData);
		if (it == exifData.end())
			throw std::runtime_error("\"" + exp.filename + "\": could not extract the exposure time!");
		exp.shown_exposure = it->toFloat();

		/* Fail if the images use different ISO values */
		it = Exiv2::isoSpeed(exifData);
		if (it == exifData.end())
			throw std::runtime_error("\"" + exp.filename + "\": could not extract the ISO speed!");
		if (exposure == 0)
			isoSpeed = it->toFloat();
		else if (isoSpeed != it->toFloat())
			throw std::runtime_error("\"" + exp.filename + "\": detected an ISO speed that is different from the other images!");

		/* Fail if the images use different aperture settings */
		it = Exiv2::fNumber(exifData);
		if (it == exifData.end())
			throw std::runtime_error("\"" + exp.filename + "\": could not extract the aperture setting!");
		if (exposure == 0)
			aperture = it->toFloat();
		else if (aperture != it->toFloat())
			throw std::runtime_error("\"" + exp.filename + "\": detected an aperture setting that is different from the other images!");

		/* Check for exposure mode, possibly warn */
		it = Exiv2::exposureMode(exifData);
		if (it == exifData.end())
			throw std::runtime_error("\"" + exp.filename + "\": could not extract the exposure mode!");
		if (it->print(&exifData) != "Manual")
			std::cerr << "Warning: image \"" << exp.filename << "\" was *not* taken in manual exposure mode!" << endl;

		/* If this image was taken by a Canon camera, also check the focus mode and possibly warn */
		it = exifData.findKey(Exiv2::ExifKey("Exif.CanonCs.FocusMode"));
		if (it != exifData.end()) {
			if (it->print(&exifData) != "Manual focus")
				std::cerr << "Warning: image \"" << exp.filename << "\" was *not* taken in manual focus mode!" << endl;
		}
	}

	std::sort(exposures.begin(), exposures.end(),
		[](const Exposure &a, const Exposure &b) {
			return a.exposure < b.exposure;
		}
	);

	cout << "Found " << exposures.size() << " image" <<
		(exposures.size() > 1 ? "s" : "");
	cout  << " [ISO " << isoSpeed << ", ";

	if (aperture == 0)
		cout << "f/unknown";
	else
		cout << "f/" << aperture;

	cout << ", exposures: ";
	for (size_t i=0; i<exposures.size(); ++i) {
		cout << exposures[i].toString();
		if (i+1 < exposures.size())
			cout << ", ";
	}

	cout << "]" << endl;

	std::vector<Exposure>::iterator it = std::adjacent_find(exposures.begin(), exposures.end(),
		[](const Exposure &a, const Exposure &b) {
			return a.exposure == b.exposure;
	});

	if (it != exposures.end())
		throw std::runtime_error((boost::format("Duplicate exposure time: %1%") % it->toString()).str());

	cout << "Collected " << metadata.size() << " metadata entries." << endl;
}

void ExposureSeries::load() {
	std::unique_ptr<CameraMetaData> metadata(new CameraMetaData("rawspeed/data/cameras.xml"));

	cout << "Loading raw image data ..";
	cout.flush();

	#pragma omp parallel for schedule(dynamic, 1)
	for (int i=0; i<(int) exposures.size(); ++i) {
		FileReader f((char *) exposures[i].filename.c_str());
		std::unique_ptr<FileMap> map(f.readFile());

		RawParser parser(map.get());
		std::unique_ptr<RawDecoder> decoder(parser.getDecoder());

		if (!decoder.get())
			throw std::runtime_error((boost::format(
				"Unable to decode RAW file \"%1%\"!") % exposures[i].filename).str());

		decoder->failOnUnknown = true;
		decoder->checkSupport(metadata.get());

		/* Decode the RAW data and crop to the active image area */
		decoder->decodeRaw();
		decoder->decodeMetaData(metadata.get());
		RawImage raw = decoder->mRaw;

		if (raw->subsampling.x != 1 || raw->subsampling.y != 1)
			throw std::runtime_error("Subsampled RAW images are currently not supported!");

		if (raw->getDataType() != TYPE_USHORT16)
			throw std::runtime_error("Only RAW data in 16-bit format is currently supported!");

		if (!raw->isCFA)
			throw std::runtime_error("Only sensors with a color filter array are currently supported!");

		ColorFilterArray cfa = raw->cfa;

		int width = raw->dim.x, height = raw->dim.y, pitch = raw->pitch / sizeof(uint16_t);
		if (i == 0) {
			this->width = width;
			this->height = height;
		}

		uint16_t *data = (uint16_t *) raw->getData(0, 0);

		uint16_t offset = raw->blackLevel;
		float factor = 1.0f / (raw->whitePoint - offset);
	
		float *image = new float[width*height];
		for (int y=0; y<height; ++y) {
			uint16_t *src = data + y*pitch;
			float *dst = image + y*width;

			for (int x=0; x<width; ++x)
				*dst++ = (*src++ - offset) * factor;
		}

		exposures[i].image = image;

		#pragma omp critical
		{
			cout << ".";
			cout.flush();
		}
	}

	cout << " done (" << width << "x" << height << ", using "
		 << (width*height*sizeof(float) * exposures.size()) / (float) (1024*1024)
		 << " MiB of memory)" << endl;

	/* Determine the value of a pixel considered to be overexposured */
	size_t npix = width*height;
	float *temp = new float[npix];
	memcpy(temp, exposures[size()-1].image, npix*sizeof(float));
	size_t percentile = (size_t) (npix*0.999);
	std::nth_element(temp, temp+percentile, temp+npix);
	saturation = *(temp+percentile);
	delete[] temp;

	cout << "Saturation detected to be around " << saturation << "." << endl;
}