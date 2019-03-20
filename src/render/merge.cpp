/*
 * Copyright 2011-2019 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "render/merge.h"

#include "util/util_array.h"
#include "util/util_map.h"
#include "util/util_system.h"
#include "util/util_time.h"
#include "util/util_unique_ptr.h"

#include <OpenImageIO/imageio.h>
#include <OpenImageIO/filesystem.h>

OIIO_NAMESPACE_USING

CCL_NAMESPACE_BEGIN

/* Merge Image Layer */

enum MergeChannelOp {
	MERGE_CHANNEL_NOP,
	MERGE_CHANNEL_COPY,
	MERGE_CHANNEL_SUM,
	MERGE_CHANNEL_AVERAGE
};

struct MergeImagePass {
	/* Full channel name. */
    string channel_name;
	/* Channel format in the file. */
    TypeDesc format;
	/* Type of operation to perform when merging. */
	MergeChannelOp op;
	/* Offset of layer channels in input image. */
	int offset;
	/* Offset of layer channels in merged image. */
	int merge_offset;
};

struct MergeImageLayer {
	/* Layer name. */
	string name;
	/* Passes. */
	vector<MergeImagePass> passes;
	/* Sample amount that was used for rendering this layer. */
	int samples;
};

/* Merge Image */

struct MergeImage {
	/* OIIO file handle. */
	unique_ptr<ImageInput> in;
	/* Image file path. */
	string filepath;
	/* Render layers. */
	vector<MergeImageLayer> layers;
};

/* Channel Parsing */

static MergeChannelOp parse_channel_operation(const string& pass_name)
{
	if(pass_name == "Depth" ||
	   pass_name == "IndexMA" ||
	   pass_name == "IndexOB" ||
	   string_startswith(pass_name, "Crypto"))
	{
		return MERGE_CHANNEL_COPY;
	}
	else if(string_startswith(pass_name, "Debug BVH") ||
	        string_startswith(pass_name, "Debug Ray") ||
	        string_startswith(pass_name, "Debug Render Time"))
	{
		return MERGE_CHANNEL_SUM;
	}
	else {
		return MERGE_CHANNEL_AVERAGE;
	}
}

/* Splits in at its last dot, setting suffix to the part after the dot and
 * into the part before it. Returns whether a dot was found. */
static bool split_last_dot(string &in, string &suffix)
{
	size_t pos = in.rfind(".");
	if(pos == string::npos) {
		return false;
	}
	suffix = in.substr(pos+1);
	in = in.substr(0, pos);
	return true;
}

/* Separate channel names as generated by Blender.
 * Multiview format: RenderLayer.Pass.View.Channel
 * Otherwise: RenderLayer.Pass.Channel */
static bool parse_channel_name(string name,
                               string &renderlayer,
                               string &pass,
                               string &channel,
                               bool multiview_channels)
{
	if(!split_last_dot(name, channel)) {
		return false;
	}
	string view;
	if(multiview_channels && !split_last_dot(name, view)) {
		return false;
	}
	if(!split_last_dot(name, pass)) {
		return false;
	}
	renderlayer = name;

	if(multiview_channels) {
		renderlayer += "." + view;
	}

	return true;
}

static bool parse_channels(const ImageSpec &in_spec,
                           vector<MergeImageLayer>& layers,
                           string& error)
{
	const ParamValue *multiview = in_spec.find_attribute("multiView");
	const bool multiview_channels = (multiview &&
	                                 multiview->type().basetype == TypeDesc::STRING &&
	                                 multiview->type().arraylen >= 2);

	layers.clear();

	/* Loop over all the channels in the file, parse their name and sort them
	 * by RenderLayer.
	 * Channels that can't be parsed are directly passed through to the output. */
	map<string, MergeImageLayer> file_layers;
	for(int i = 0; i < in_spec.nchannels; i++) {
		MergeImagePass pass;
		pass.channel_name = in_spec.channelnames[i];
		pass.format = (in_spec.channelformats.size() > 0) ? in_spec.channelformats[i] : in_spec.format;
		pass.offset = i;
		pass.merge_offset = i;

		string layername, passname, channelname;
		if(parse_channel_name(pass.channel_name, layername, passname, channelname, multiview_channels)) {
			/* Channer part of a render layer. */
			pass.op = parse_channel_operation(passname);
		}
		else {
			/* Other channels are added in unnamed layer. */
			layername = "";
			pass.op = parse_channel_operation(pass.channel_name);
		}

		file_layers[layername].passes.push_back(pass);
	}

	/* Loop over all detected RenderLayers, check whether they contain a full set of input channels.
	 * Any channels that won't be processed internally are also passed through. */
	for(auto& i: file_layers) {
		const string& name = i.first;
		MergeImageLayer& layer = i.second;

		layer.name = name;
		layer.samples = 0;

		/* Determine number of samples from metadata. */
		if(layer.name == "") {
			layer.samples = 1;
		}
		else if(layer.samples < 1) {
			string sample_string = in_spec.get_string_attribute("cycles." + name + ".samples", "");
			if(sample_string != "") {
				if(!sscanf(sample_string.c_str(), "%d", &layer.samples)) {
					error = "Failed to parse samples metadata: " + sample_string;
					return false;
				}
			}
		}

		if(layer.samples < 1) {
			error = string_printf("No sample number specified in the file for layer %s or on the command line", name.c_str());
			return false;
		}

		layers.push_back(layer);
	}

	return true;
}

static bool open_images(const vector<string>& filepaths,
                        vector<MergeImage>& images,
                        string& error)
{
	for(const string& filepath: filepaths) {
		unique_ptr<ImageInput> in(ImageInput::open(filepath));
		if(!in) {
			error = "Couldn't open file: " + filepath;
			return false;
		}

		MergeImage image;
		image.in = std::move(in);
		image.filepath = filepath;
		if(!parse_channels(image.in->spec(), image.layers, error)) {
			return false;
		}

		if(image.layers.size() == 0) {
			error = "Could not find a render layer for merging";
			return false;
		}

		if(image.in->spec().deep) {
			error = "Merging deep images not supported.";
			return false;
		}

		if(images.size() > 0) {
			const ImageSpec& base_spec = images[0].in->spec();
			const ImageSpec& spec = image.in->spec();

			if(base_spec.width != spec.width ||
			   base_spec.height != spec.height ||
			   base_spec.depth != spec.depth ||
			   base_spec.format != spec.format ||
			   base_spec.deep != spec.deep)
			{
				error = "Images do not have matching size and data layout.";
				return false;
			}
		}

		images.push_back(std::move(image));
	}

	return true;
}

static void merge_render_time(ImageSpec& spec,
                              const vector<MergeImage>& images,
                              const string& name,
                              const bool average)
{
	double time = 0.0;

	for(const MergeImage& image: images) {
		string time_str = image.in->spec().get_string_attribute(name, "");
		time += time_human_readable_to_seconds(time_str);
	}

	if(average) {
		time /= images.size();
	}

	spec.attribute(name, TypeDesc::STRING, time_human_readable_from_seconds(time));
}

static void merge_layer_render_time(ImageSpec& spec,
                                    const vector<MergeImage>& images,
                                    const string& layer_name,
                                    const string& time_name,
                                    const bool average)
{
	string name = "cycles." + layer_name + "." + time_name;
	double time = 0.0;

	for(const MergeImage& image: images) {
		string time_str = image.in->spec().get_string_attribute(name, "");
		time += time_human_readable_to_seconds(time_str);
	}

	if(average) {
		time /= images.size();
	}

	spec.attribute(name, TypeDesc::STRING, time_human_readable_from_seconds(time));
}

static void merge_channels_metadata(vector<MergeImage>& images,
                                    ImageSpec& out_spec,
                                    vector<int>& channel_total_samples)
{
	/* Based on first image. */
	out_spec  = images[0].in->spec();

	/* Merge channels and compute offsets. */
	out_spec.nchannels = 0;
	out_spec.channelformats.clear();
	out_spec.channelnames.clear();

	for(MergeImage& image: images) {
		for(MergeImageLayer& layer: image.layers) {
			for(MergeImagePass& pass: layer.passes) {
				/* Test if matching channel already exists in merged image. */
				bool found = false;

				for(size_t i = 0; i < out_spec.nchannels; i++) {
					if(pass.channel_name == out_spec.channelnames[i]) {
						pass.merge_offset = i;
						channel_total_samples[i] += layer.samples;
						/* First image wins for channels that can't be averaged or summed. */
						if (pass.op == MERGE_CHANNEL_COPY) {
							pass.op = MERGE_CHANNEL_NOP;
						}
						found = true;
						break;
					}
				}

				if(!found) {
					/* Add new channel. */
					pass.merge_offset = out_spec.nchannels;
					channel_total_samples.push_back(layer.samples);

					out_spec.channelnames.push_back(pass.channel_name);
					out_spec.channelformats.push_back(pass.format);
					out_spec.nchannels++;
				}
			}
		}
	}

	/* Merge metadata. */
	merge_render_time(out_spec, images, "RenderTime", false);

	map<string, int> layer_num_samples;
	for(MergeImage& image: images) {
		for(MergeImageLayer& layer: image.layers) {
			if(layer.name != "") {
				layer_num_samples[layer.name] += layer.samples;
			}
		}
	}

	for(const auto& i: layer_num_samples) {
		string name = "cycles." + i.first + ".samples";
		out_spec.attribute(name, TypeDesc::STRING, string_printf("%d", i.second));

		merge_layer_render_time(out_spec, images, i.first, "total_time", false);
		merge_layer_render_time(out_spec, images, i.first, "render_time", false);
		merge_layer_render_time(out_spec, images, i.first, "synchronization_time", true);
	}
}

static void alloc_pixels(const ImageSpec& spec, array<float>& pixels)
{
	const size_t width = spec.width;
	const size_t height = spec.height;
	const size_t num_channels = spec.nchannels;

	const size_t num_pixels = (size_t)width * (size_t)height;
	pixels.resize(num_pixels * num_channels);
}

static bool merge_pixels(const vector<MergeImage>& images,
                         const ImageSpec& out_spec,
                         const vector<int>& channel_total_samples,
                         array<float>& out_pixels,
                         string& error)
{
	alloc_pixels(out_spec, out_pixels);
	memset(out_pixels.data(), 0, out_pixels.size() * sizeof(float));

	for(const MergeImage& image: images) {
		/* Read all channels into buffer. Reading all channels at once is
		 * faster than individually due to interleaved EXR channel storage. */
		array<float> pixels;
		alloc_pixels(image.in->spec(), pixels);

		if(!image.in->read_image(TypeDesc::FLOAT, pixels.data())) {
			error = "Failed to read image: " + image.filepath;
			return false;
		}

		for(size_t li = 0; li < image.layers.size(); li++) {
			const MergeImageLayer& layer = image.layers[li];

			const size_t stride = out_spec.nchannels;
			const size_t num_pixels = pixels.size();

			for(const MergeImagePass& pass: layer.passes) {
				size_t offset = pass.offset;
				size_t merge_offset = pass.merge_offset;

				switch(pass.op) {
					case MERGE_CHANNEL_NOP:
						break;
					case MERGE_CHANNEL_COPY:
						for(size_t i = 0; i < num_pixels; i += stride) {
							out_pixels[i + merge_offset] = pixels[i + offset];
						}
						break;
					case MERGE_CHANNEL_SUM:
						for(size_t i = 0; i < num_pixels; i += stride) {
							out_pixels[i + merge_offset] += pixels[i + offset];
						}
						break;
					case MERGE_CHANNEL_AVERAGE:
						/* Weights based on sample metadata. Per channel since not
						 * all files are guaranteed to have the same channels. */
						const int total_samples = channel_total_samples[offset];
						const float t = (float)layer.samples / (float)total_samples;

						for(size_t i = 0; i < num_pixels; i += stride) {
							out_pixels[i + merge_offset] += t * pixels[i + offset];
						}
						break;
				}
			}
		}
	}

	return true;
}

static bool save_output(const string& filepath,
                        const ImageSpec& spec,
                        const array<float>& pixels,
                        string& error)
{
	/* Write to temporary file path, so we merge images in place and don't
	 * risk destroying files when something goes wrong in file saving. */
	string extension = OIIO::Filesystem::extension(filepath);
	string unique_name = ".merge-tmp-" + OIIO::Filesystem::unique_path();
	string tmp_filepath = filepath + unique_name + extension;
	unique_ptr<ImageOutput> out(ImageOutput::create(tmp_filepath));

	if(!out) {
		error = "Failed to open temporary file " + tmp_filepath + " for writing";
		return false;
	}

	/* Open temporary file and write image buffers. */
	if(!out->open(tmp_filepath, spec)) {
		error = "Failed to open file " + tmp_filepath + " for writing: " + out->geterror();
		return false;
	}

	bool ok = true;
	if(!out->write_image(TypeDesc::FLOAT, pixels.data())) {
		error = "Failed to write to file " + tmp_filepath + ": " + out->geterror();
		ok = false;
	}

	if(!out->close()) {
		error = "Failed to save to file " + tmp_filepath + ": " + out->geterror();
		ok = false;
	}

	out.reset();

	/* Copy temporary file to outputput filepath. */
	string rename_error;
	if(ok && !OIIO::Filesystem::rename(tmp_filepath, filepath, rename_error)) {
		error = "Failed to move merged image to " + filepath + ": " + rename_error;
		ok = false;
	}

	if(!ok) {
		OIIO::Filesystem::remove(tmp_filepath);
	}

	return ok;
}

/* Image Merger */

ImageMerger::ImageMerger()
{
}

bool ImageMerger::run()
{
	if(input.empty()) {
		error = "No input file paths specified.";
		return false;
	}
	if(output.empty()) {
		error = "No output file path specified.";
		return false;
	}

	/* Open images and verify they have matching layout. */
	vector<MergeImage> images;
	if(!open_images(input, images, error)) {
		return false;
	}

	/* Merge metadata and setup channels and offsets. */
	ImageSpec out_spec;
	vector<int> channel_total_samples;
	merge_channels_metadata(images, out_spec, channel_total_samples);

	/* Merge pixels. */
	array<float> out_pixels;
	if(!merge_pixels(images, out_spec, channel_total_samples, out_pixels, error)) {
		return false;
	}

	/* We don't need input anymore at this point, and will possibly
	 * overwrite the same file. */
	images.clear();

	/* Save output file. */
	return save_output(output, out_spec, out_pixels, error);
}

CCL_NAMESPACE_END
