/* ---------------------------------------------------------------------------
** This software is in the public domain, furnished "as is", without technical
** support, and with no warranty, express or implied, as to its usefulness for
** any purpose.
**
** v4l2web.cpp
** 
** -------------------------------------------------------------------------*/

#include <unistd.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/version.h>
#include <linux/videodev2.h>

#include <stdexcept>
	
#include <json/json.h>
#include "mongoose.h"

#include "V4l2Capture.h"
#include "v4l2web.h"

unsigned int add_ctrl(int fd, unsigned int i, Json::Value & json) 
{
	unsigned int ret=0;
	struct v4l2_queryctrl   qctrl;
	memset(&qctrl,0,sizeof(qctrl));
	qctrl.id = i;
	if (0 == ioctl(fd,VIDIOC_QUERYCTRL,&qctrl))
	{
		if (!(qctrl.flags & V4L2_CTRL_FLAG_DISABLED))
		{
			struct v4l2_control control;  
			memset(&control,0,sizeof(control));
			control.id = qctrl.id;
			if (0 == ioctl(fd,VIDIOC_G_CTRL,&control))
			{
				Json::Value value;
				value["id"           ] = control.id;
				value["name"         ] = (const char*)qctrl.name;
				value["type"         ] = qctrl.type;
				value["minimum"      ] = qctrl.minimum;
				value["maximum"      ] = qctrl.maximum;
				value["step"         ] = qctrl.step;
				value["default_value"] = qctrl.default_value;
				value["value"        ] = control.value;

				Json::Value flags;
				if (qctrl.flags & V4L2_CTRL_FLAG_DISABLED  ) flags.append("V4L2_CTRL_FLAG_DISABLED"  );
				if (qctrl.flags & V4L2_CTRL_FLAG_GRABBED   ) flags.append("V4L2_CTRL_FLAG_GRABBED"   );
				if (qctrl.flags & V4L2_CTRL_FLAG_READ_ONLY ) flags.append("V4L2_CTRL_FLAG_READ_ONLY" );
				if (qctrl.flags & V4L2_CTRL_FLAG_UPDATE    ) flags.append("V4L2_CTRL_FLAG_UPDATE"    );
				if (qctrl.flags & V4L2_CTRL_FLAG_SLIDER    ) flags.append("V4L2_CTRL_FLAG_SLIDER"    );
				if (qctrl.flags & V4L2_CTRL_FLAG_WRITE_ONLY) flags.append("V4L2_CTRL_FLAG_WRITE_ONLY");
				value["flags"]   = flags;
				
				if ( (qctrl.type == V4L2_CTRL_TYPE_MENU) 
#ifdef V4L2_CTRL_TYPE_INTEGER_MENU 
					|| (qctrl.type == V4L2_CTRL_TYPE_INTEGER_MENU) 
#endif
				   )
				{
					Json::Value menu;
					struct v4l2_querymenu querymenu;
					memset(&querymenu,0,sizeof(querymenu));
					querymenu.id = qctrl.id;
					for (querymenu.index = 0; querymenu.index <= qctrl.maximum; querymenu.index++) 
					{
						if (0 == ioctl(fd,VIDIOC_QUERYMENU,&querymenu))
						{
							Json::Value label;
							label["value"] = querymenu.index;
							if (qctrl.type == V4L2_CTRL_TYPE_MENU)
							{
								label["label"] = (const char*)querymenu.name;
							}
#ifdef V4L2_CTRL_TYPE_INTEGER_MENU 
							else if (qctrl.type == V4L2_CTRL_TYPE_INTEGER_MENU)
							{
								label["label"] = (int)querymenu.value;
							}
#endif
							menu.append(label);
						}
					}
					value["menu"] = menu;
				}
				json.append(value);
			}
		}
		ret = qctrl.id;
	}
	return ret;
}

std::string get_fourcc(unsigned int pixelformat)
{
	std::string fourcc;
	fourcc.append(1, pixelformat&0xff);
	fourcc.append(1, (pixelformat>>8)&0xff);
	fourcc.append(1, (pixelformat>>16)&0xff);
	fourcc.append(1, (pixelformat>>24)&0xff);
	return fourcc;
}

static int send_capabilities_reply(struct mg_connection *conn) 
{
	V4l2Capture* dev =(V4l2Capture*)conn->server_param;
	int fd = dev->getFd();		
	Json::Value json;
	v4l2_capability cap;
	memset(&cap,0,sizeof(cap));
	if (-1 != ioctl(fd,VIDIOC_QUERYCAP,&cap))
	{
		json["driver"]     = (const char*)cap.driver;
		json["card"]       = (const char*)cap.card;
		json["bus_info"]   = (const char*)cap.bus_info;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,3,0)		
		Json::Value capabilities;
		if (cap.device_caps & V4L2_CAP_VIDEO_CAPTURE) capabilities.append("V4L2_CAP_VIDEO_CAPTURE");
		if (cap.device_caps & V4L2_CAP_VIDEO_OUTPUT ) capabilities.append("V4L2_CAP_VIDEO_OUTPUT" );
		if (cap.device_caps & V4L2_CAP_READWRITE    ) capabilities.append("V4L2_CAP_READWRITE"    );
		if (cap.device_caps & V4L2_CAP_ASYNCIO      ) capabilities.append("V4L2_CAP_ASYNCIO"      );
		if (cap.device_caps & V4L2_CAP_STREAMING    ) capabilities.append("V4L2_CAP_STREAMING"    );
		json["capabilities"]   = capabilities;
#endif		
	}
	Json::StyledWriter styledWriter;
	std::string str (styledWriter.write(json));
	mg_printf_data(conn, str.c_str());		
	return MG_TRUE;
}

static int send_inputs_reply(struct mg_connection *conn) 
{
	V4l2Capture* dev =(V4l2Capture*)conn->server_param;
	int fd = dev->getFd();		
	Json::Value json;
	for (int i = 0;; i++) 
	{
		v4l2_input input;
		memset(&input,0,sizeof(input));
		input.index = i;
		if (-1 == ioctl(fd,VIDIOC_ENUMINPUT,&input))
			break;
		
		Json::Value value;
		value["name"]   = (const char*)input.name;
		switch (input.type)
		{
			case V4L2_INPUT_TYPE_TUNER : value["type"] = "V4L2_INPUT_TYPE_TUNER" ; break;
			case V4L2_INPUT_TYPE_CAMERA: value["type"] = "V4L2_INPUT_TYPE_CAMERA"; break;
			default : break;
		}		
		Json::Value status;
		if (input.status & V4L2_IN_ST_NO_POWER ) status.append("V4L2_IN_ST_NO_POWER" );
		if (input.status & V4L2_IN_ST_NO_SIGNAL) status.append("V4L2_IN_ST_NO_SIGNAL");
		if (input.status & V4L2_IN_ST_NO_COLOR ) status.append("V4L2_IN_ST_NO_COLOR" );
		value["status"] = status;			
		json.append(value);
	}
	Json::StyledWriter styledWriter;
	std::string str (styledWriter.write(json));
	mg_printf_data(conn, str.c_str());		
	return MG_TRUE;
}

void add_frameIntervals(int fd, unsigned int pixelformat, unsigned int width, unsigned int height, Json::Value & frameSize) 
{
	Json::Value frameIntervals;
	struct v4l2_frmivalenum frmival;
	frmival.index = 0;
	frmival.pixel_format = pixelformat;
	frmival.width = width;
	frmival.height = height;
	while (ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmival) == 0) 
	{
		Json::Value frameInter;
		if (frmival.type == V4L2_FRMIVAL_TYPE_DISCRETE) 
		{
			frameInter["fps"] = 1.0*frmival.discrete.denominator/frmival.discrete.numerator;
		}
		else
		{
			Json::Value fps;
			fps["min"]  = frmival.stepwise.max.denominator/frmival.stepwise.max.numerator;
			fps["max"]  = frmival.stepwise.min.denominator/frmival.stepwise.min.numerator;
			fps["step"] = frmival.stepwise.step.denominator/frmival.stepwise.step.numerator;
			frameInter["fps"] = fps;
		}
		frameIntervals.append(frameInter);
		frmival.index++;
	}
	frameSize["intervals"] = frameIntervals;
}
				
static int send_formats_reply(struct mg_connection *conn) 
{
	V4l2Capture* dev =(V4l2Capture*)conn->server_param;
	int fd = dev->getFd();		
	Json::Value json;
	for (int i = 0;; i++) 
	{
		struct v4l2_fmtdesc     fmtdesc;
		memset(&fmtdesc,0,sizeof(fmtdesc));
		fmtdesc.index = i;
		fmtdesc.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (-1 == ioctl(fd,VIDIOC_ENUM_FMT,&fmtdesc))
			break;
		
		Json::Value value;
		value["description"] = (const char*)fmtdesc.description;
		value["type"]        = fmtdesc.type;
		value["format"]      = get_fourcc(fmtdesc.pixelformat);		

		Json::Value frameSizeList;
		struct v4l2_frmsizeenum frmsize;
		memset(&frmsize,0,sizeof(frmsize));
		frmsize.pixel_format = fmtdesc.pixelformat;
		frmsize.index = 0;
		while (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0) 
		{
			if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) 
			{				
				Json::Value frameSize;
				frameSize["width"] = frmsize.discrete.width;
				frameSize["height"] = frmsize.discrete.height;
				add_frameIntervals(fd, frmsize.pixel_format, frmsize.discrete.width, frmsize.discrete.height, frameSize);
				frameSizeList.append(frameSize);
			}
			else 
			{
				Json::Value frameSize;				
				Json::Value width;
				width["min"] = frmsize.stepwise.min_width;
				width["max"] = frmsize.stepwise.max_width;
				width["step"] = frmsize.stepwise.step_width;				
				frameSize["width"] = width;
				
				Json::Value height;
				height["min"] = frmsize.stepwise.min_height;
				height["max"] = frmsize.stepwise.max_height;
				height["step"] = frmsize.stepwise.step_height;				
				frameSize["height"] = height;
				
				add_frameIntervals(fd, frmsize.pixel_format, frmsize.stepwise.max_width, frmsize.stepwise.max_height, frameSize);
				frameSizeList.append(frameSize);				
			}
			frmsize.index++;
		}
		value["frameSizes"]     = frameSizeList;

		json.append(value);
	}
	Json::StyledWriter styledWriter;
	std::string str (styledWriter.write(json));
	mg_printf_data(conn, str.c_str());		
	return MG_TRUE;
}

static int send_format_reply(struct mg_connection *conn) 
{
	V4l2Capture* dev =(V4l2Capture*)conn->server_param;
	int fd = dev->getFd();		
	Json::Value output;
	
	// set format POST
	if ( (conn->content_len != 0) && (conn->content != NULL) )
	{
		std::string content(conn->content, conn->content_len);
		Json::Value input;
		Json::Reader reader;
		reader.parse(content,input);
		
		Json::StyledWriter writer;
		std::cout << writer.write(input) << std::endl;		
		
		struct v4l2_format     format;
		memset(&format,0,sizeof(format));
		format.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (0 == ioctl(fd,VIDIOC_G_FMT,&format))
		{
			try
			{
				format.fmt.pix.width = input.get("width",format.fmt.pix.width).asUInt();
				format.fmt.pix.height = input.get("height",format.fmt.pix.height).asUInt();
				std::string formatstr = input.get("format","").asString();
				if (!formatstr.empty())
				{
					const char* fourcc[4];
					memset(&fourcc,0, sizeof(fourcc));
					unsigned len = sizeof(format);
					if (formatstr.size()<len) len = formatstr.size();
					memcpy(&fourcc,formatstr.c_str(),len);
					format.fmt.pix.pixelformat = v4l2_fourcc(fourcc[0],fourcc[1],fourcc[2],fourcc[3]);
				}
				errno=0;
				output["ioctl"] = ioctl(fd,VIDIOC_S_FMT,&format);
				output["errno"]  = errno;
				output["error"]  = strerror(errno);	
			}
			catch (const std::runtime_error &e)
			{
				output["exception"]  = e.what();
			}			
		}		
		struct v4l2_streamparm parm;
		memset(&parm,0,sizeof(parm));
		parm.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (0 == ioctl(fd,VIDIOC_G_PARM,&parm))
		{
			if (parm.parm.capture.timeperframe.numerator != 0)
			{
				try
				{
					parm.parm.capture.timeperframe.denominator=input.get("fps",parm.parm.capture.timeperframe.denominator/parm.parm.capture.timeperframe.numerator).asUInt();
					parm.parm.capture.timeperframe.numerator=1;
					errno=0;
					output["ioctl"] = ioctl(fd,VIDIOC_S_PARM,&parm);
					output["errno"]  = errno;
					output["error"]  = strerror(errno);	
				}
				catch (const std::runtime_error &e)
				{
					output["exception"]  = e.what();
				}			
			}
		}
	}

	// query the format
	struct v4l2_format     format;
	memset(&format,0,sizeof(format));
	format.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (0 == ioctl(fd,VIDIOC_G_FMT,&format))
	{
		output["width"]     = format.fmt.pix.width;
		output["height"]    = format.fmt.pix.height;
		output["sizeimage"] = format.fmt.pix.sizeimage;
		output["format"]    = get_fourcc(format.fmt.pix.pixelformat);			
		
	}
	struct v4l2_streamparm parm;
	memset(&parm,0,sizeof(parm));
	parm.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (0 == ioctl(fd,VIDIOC_G_PARM,&parm))
	{
		Json::Value capabilities;
		if (parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) capabilities.append("V4L2_CAP_TIMEPERFRAME");
		output["capabilities"]  = capabilities;		
		Json::Value capturemode;
		if (parm.parm.capture.capturemode & V4L2_MODE_HIGHQUALITY) capturemode.append("V4L2_MODE_HIGHQUALITY");
		output["capturemode"]   = capturemode;		
		output["readbuffers"]   = parm.parm.capture.readbuffers;		
		double fps = 0.0;
		if (parm.parm.capture.timeperframe.numerator != 0) fps = 1.0*parm.parm.capture.timeperframe.denominator/parm.parm.capture.timeperframe.numerator;
		output["fps"]           = fps; 
	}
	
	Json::StyledWriter styledWriter;
	std::string str (styledWriter.write(output));
	std::cerr << str << std::endl;	
	mg_printf_data(conn, str.c_str());
	return MG_TRUE;
}

static int send_controls_reply(struct mg_connection *conn) 
{
	V4l2Capture* dev =(V4l2Capture*)conn->server_param;
	int fd = dev->getFd();		
	Json::Value json;
	for (unsigned int i = V4L2_CID_BASE; i<V4L2_CID_LASTP1; add_ctrl(fd,i,json), i++);
	for (unsigned int i = V4L2_CID_LASTP1+1; i != 0 ; i=add_ctrl(fd,i|V4L2_CTRL_FLAG_NEXT_CTRL,json));
	Json::StyledWriter styledWriter;
	std::string str (styledWriter.write(json));
	mg_printf_data(conn, str.c_str());		
	return MG_TRUE;
}

static int send_control_reply(struct mg_connection *conn) 
{
	V4l2Capture* dev =(V4l2Capture*)conn->server_param;
	int fd = dev->getFd();		
	Json::Value output;
		
	if ( (conn->content_len != 0) && (conn->content != NULL) )
	{
		std::string content(conn->content, conn->content_len);
		Json::Value input;
		Json::Reader reader;
		reader.parse(content,input);
		
		Json::StyledWriter writer;
		std::cout << writer.write(input) << std::endl;		
			
		try
		{
			unsigned int key = input["id"].asUInt();
			
			struct v4l2_control control;  
			memset(&control,0,sizeof(control));
			control.id = key;
			if (input.isMember("value"))
			{
				int value = input["value"].asInt();
				control.value = value;
				errno=0;
				output["ioctl"] = ioctl(fd,VIDIOC_S_CTRL,&control);
				output["errno"]  = errno;
				output["error"]  = strerror(errno);
			}
			else
			{
				errno=0;
				output["ioctl"] = ioctl(fd,VIDIOC_G_CTRL,&control);
				output["errno"]  = errno;
				output["error"]  = strerror(errno);
			}
			output["id"] = control.id;
			output["value"] = control.value;
			
		}
		catch (const std::runtime_error &e)
		{
			output["exception"]  = e.what();
		}
		
	}
	Json::StyledWriter styledWriter;
	std::string str (styledWriter.write(output));
	std::cerr << str << std::endl;		
	mg_printf_data(conn, str.c_str());		
	return MG_TRUE;
}

static int send_ws_reply(struct mg_connection *conn) 
{
	int ret = MG_FALSE;
	if (conn->is_websocket) 
	{			
		mg_websocket_write(conn, WEBSOCKET_OPCODE_TEXT, conn->content, conn->content_len);
		ret=MG_TRUE;
	} 
	return ret;
}

int send_ws_notif(struct mg_connection *conn, char* buffer, ssize_t size) 
{
	if (conn->is_websocket) 
	{
		mg_websocket_write(conn, WEBSOCKET_OPCODE_BINARY, buffer, size);
	}
	return MG_TRUE;
}

static int send_start_reply(struct mg_connection *conn) 
{
	V4l2Capture* dev =(V4l2Capture*)conn->server_param;
	mg_printf_data(conn, "%d", dev->captureStart());
	return MG_TRUE;
}

static int send_stop_reply(struct mg_connection *conn) 
{
	V4l2Capture* dev =(V4l2Capture*)conn->server_param;
	mg_printf_data(conn, "%d", dev->captureStop());
	return MG_TRUE;
}

static int send_isCapturing_reply(struct mg_connection *conn) 
{
	V4l2Capture* dev =(V4l2Capture*)conn->server_param;
	mg_printf_data(conn, "%d", dev->isReady());
	return MG_TRUE;
}

static int send_jpeg_reply(struct mg_connection *conn) 
{
	mg_send_header(conn, "Cache-Control", "no-cache");
	mg_send_header(conn, "Pragma", "no-cache");
	mg_send_header(conn, "Expires", "Thu, 01 Dec 1994 16:00:00 GMT");
	mg_send_header(conn, "Connection", "close");
	mg_send_header(conn, "Content-Type", "multipart/x-mixed-replace; boundary=--myboundary");
	mg_write(conn,"\r\n",2);
	send_start_reply(conn);
	return MG_MORE;
}

int send_jpeg_notif(struct mg_connection *conn, char* buffer, ssize_t size) 
{
	mg_printf(conn, "--myboundary\r\nContent-Type: image/jpeg\r\n"
		"Content-Length: %llu\r\n\r\n", (unsigned long long) size);
	mg_write(conn, buffer, size);
	mg_write(conn,"\r\n",2);
	return MG_TRUE;
}

int send_help_reply(struct mg_connection *conn);	
url_handler urls[] = {
	{ "/capabilities", send_capabilities_reply, NULL, NULL },
	{ "/inputs", send_inputs_reply, NULL, NULL },
	{ "/formats", send_formats_reply, NULL, NULL },
	{ "/format", send_format_reply, NULL, NULL },
	{ "/controls", send_controls_reply, NULL, NULL },
	{ "/control", send_control_reply, NULL, NULL },
	{ "/ws", send_ws_reply, NULL, send_ws_notif },
	{ "/jpeg", send_jpeg_reply, send_stop_reply, send_jpeg_notif },
	{ "/start", send_start_reply, NULL, NULL },
	{ "/stop", send_stop_reply, NULL, NULL },
	{ "/isCapturing", send_isCapturing_reply, NULL, NULL },
	{ "/help", send_help_reply, NULL, NULL },
	{ NULL, NULL, NULL, NULL },
};
int send_help_reply(struct mg_connection *conn) 
{
	mg_send_header(conn, "Cache-Control", "no-cache");
	for (int i=0; urls[i].uri ; ++i)
	{
		mg_printf_data(conn, "%s\n", urls[i].uri);	
	}
	return MG_TRUE;
}

const url_handler* find_url(const char* uri)
{
	const url_handler* url = NULL;
	if (uri != NULL)
	{
		for (int i=0; urls[i].uri ; ++i)
		{
			if (strcmp(urls[i].uri, uri) == 0)
			{
				url = &urls[i];
				break;
			}
		}
	}
	return url;
}

