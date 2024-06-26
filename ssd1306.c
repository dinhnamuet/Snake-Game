#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/random.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/moduleparam.h>
#include "ssd1306.h"

#define BUTTON_UP 1
#define BUTTON_DOWN 0
#define BUTTON_LEFT 3
#define BUTTON_RIGHT 2

const int max_X = OLED_WIDTH / FONT_X;
const int max_Y = OLED_HEIGHT / 8;
const int frame_size = FONT_X * max_X * max_Y;

static u32 speed = 4;
module_param(speed, uint, S_IRUGO);
MODULE_PARM_DESC(speed, "Speed of Snake");

struct ssd1306
{
	struct i2c_client *client;
	u8 current_X;
	u8 current_Y;
	struct work_struct workqueue;
	struct timer_list my_timer;

	struct gpio_desc *up;
	struct gpio_desc *down;
	struct gpio_desc *left;
	struct gpio_desc *right;

	u8 *frame_buffer;
	int current_index;

	/* Game Area */
	bool gameover;
	control_t button;
	int button_irq[4];
	u32 score;
	struct snake *mySnake;
	struct food myFood;
	u8 current_length;
};

static void ssd1306_write(struct ssd1306 *oled, u8 data, write_mode_t mode);
static void ssd1306_init(struct ssd1306 *oled);
static void ssd1306_clear(struct ssd1306 *oled);
static void ssd1306_goto_xy(struct ssd1306 *oled, u8 x, u8 y);
static void ssd1306_send_char(struct ssd1306 *oled, u8 data);
static void ssd1306_send_char_inv(struct ssd1306 *oled, u8 data);
static void ssd1306_send_string(struct ssd1306 *oled, u8 *str, color_t color);
static void ssd1306_go_to_next_line(struct ssd1306 *oled);
static int ssd1306_burst_write(struct ssd1306 *oled, const u8 *data, int len, write_mode_t mode);
static void ssd1306_sync(struct ssd1306 *oled);
static int embedded_to_buffer(struct ssd1306 *oled, u8 *data, int start, int length);

static void animation(struct work_struct *work);
static void tmHandler(struct timer_list *tm);
irqreturn_t buttonHandler(int irq, void *dev_id);

/* Snake Game Area */
static void snake_update_score(struct ssd1306 *oled, u32 score);
static void snake_game_setup(struct ssd1306 *oled);
static void snake_game_draw(struct ssd1306 *oled);
static void snake_game_logic(struct ssd1306 *oled);
static u8 create_random_number(u8 MAX);
static int add_new_element(struct ssd1306 *oled);
static void move(struct snake *snk);

static int oled_probe(struct i2c_client *client)
{
	int i;
	struct device *dev = NULL;
	char *label[] = {
		"button-up",
		"button-down",
		"button-left",
		"button-right"};
	struct ssd1306 *oled = NULL;
	oled = devm_kzalloc(&client->dev, sizeof(*oled), GFP_KERNEL);
	if (!oled)
	{
		pr_err("kzalloc failed\n");
		return -ENOMEM;
	}
	oled->client = client;
	dev = &client->dev;
	i2c_set_clientdata(client, oled);
	ssd1306_init(oled);
	oled->current_length = 1;
	oled->frame_buffer = kzalloc(frame_size, GFP_KERNEL);
	if (!oled->frame_buffer)
		return -ENOMEM;
	oled->mySnake = kzalloc(oled->current_length * sizeof(struct snake), GFP_KERNEL);
	if (!oled->mySnake)
		goto free_frame;
	oled->up = gpiod_get_index(dev, "buttons", BUTTON_UP, GPIOD_IN);
	oled->down = gpiod_get_index(dev, "buttons", BUTTON_DOWN, GPIOD_IN);
	oled->left = gpiod_get_index(dev, "buttons", BUTTON_LEFT, GPIOD_IN);
	oled->right = gpiod_get_index(dev, "buttons", BUTTON_RIGHT, GPIOD_IN);
	oled->button_irq[0] = gpiod_to_irq(oled->up);
	oled->button_irq[1] = gpiod_to_irq(oled->down);
	oled->button_irq[2] = gpiod_to_irq(oled->left);
	oled->button_irq[3] = gpiod_to_irq(oled->right);

	for (i = 0; i < 4; i++)
	{
		if (request_irq(oled->button_irq[i], buttonHandler, IRQF_TRIGGER_FALLING | IRQF_SHARED, label[i], oled) < 0)
		{
			pr_err("request irq gpio %d failed!\n", i);
			goto free_snake;
		}
	}
	oled->current_index = 0;
	snake_game_setup(oled);
	snake_game_draw(oled);

	INIT_WORK(&oled->workqueue, animation);
	timer_setup(&oled->my_timer, tmHandler, 0);
	oled->my_timer.expires = jiffies + HZ;
	add_timer(&oled->my_timer);
	pr_info("Start game, speed is: %d\n", speed);
	return 0;
free_snake:
	kfree(oled->mySnake);
free_frame:
	kfree(oled->frame_buffer);
	return -EFAULT;
}
static void oled_remove(struct i2c_client *client)
{
	int i;
	struct ssd1306 *oled = i2c_get_clientdata(client);
	if (!oled)
	{
		pr_err("Cannot get data\n");
	}
	else
	{
		for (i = 0; i < 4; i++)
			free_irq(oled->button_irq[i], oled);
		gpiod_put(oled->up);
		gpiod_put(oled->down);
		gpiod_put(oled->left);
		gpiod_put(oled->right);
		cancel_work_sync(&oled->workqueue);
		del_timer(&oled->my_timer);
		kfree(oled->frame_buffer);
		kfree(oled->mySnake);
		ssd1306_clear(oled);
		ssd1306_write(oled, 0xAE, COMMAND); // display off
	}
}
static const struct i2c_device_id oled_device_id[] = {
	{.name = "nam", 0},
	{}};
MODULE_DEVICE_TABLE(i2c, oled_device_id);
static const struct of_device_id oled_of_match_id[] = {
	{
		.compatible = "ssd1306-oled,nam",
	},
	{}};
MODULE_DEVICE_TABLE(of, oled_of_match_id);

static struct i2c_driver oled_driver = {
	.probe = oled_probe,
	.remove = oled_remove,
	.driver = {
		.name = "oled",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(oled_of_match_id),
	},
	.id_table = oled_device_id,
};

module_i2c_driver(oled_driver);

static void ssd1306_write(struct ssd1306 *oled, u8 data, write_mode_t mode)
{
	/*
	A control byte mainly consists of Co and D/C# bits following by six “0”
	Co | D/C | 000000
	Co bit is equal to 0
	*/
	u8 buff[2];
	if (mode == DATA)
		buff[0] = 0x40; // data
	else
		buff[0] = 0x00; // command
	buff[1] = data;
	i2c_master_send(oled->client, buff, 2);
}
static int ssd1306_burst_write(struct ssd1306 *oled, const u8 *data, int len, write_mode_t mode)
{
	int res;
	u8 *buff;
	buff = kmalloc(len + 1, GFP_KERNEL);
	if (!buff)
		return -1;
	memset(buff, 0, len + 1);
	if (mode == DATA)
		buff[0] = 0x40; // data
	else
		buff[0] = 0x00; // command
	memcpy(&buff[1], data, len);
	res = i2c_master_send(oled->client, buff, len + 1);
	kfree(buff);
	return res;
}
static void ssd1306_init(struct ssd1306 *oled)
{
	msleep(15);
	// set Osc Frequency
	ssd1306_write(oled, 0xD5, COMMAND);
	ssd1306_write(oled, 0x80, COMMAND);
	// set MUX Ratio
	ssd1306_write(oled, 0xA8, COMMAND);
	ssd1306_write(oled, 0x3F, COMMAND);
	// set display offset
	ssd1306_write(oled, 0xD3, COMMAND);
	ssd1306_write(oled, 0x00, COMMAND);
	// set display start line
	ssd1306_write(oled, 0x40, COMMAND);
	// Enable charge pump regulator
	ssd1306_write(oled, 0x8D, COMMAND);
	ssd1306_write(oled, 0x14, COMMAND);
	// Set memory addressing mode
	ssd1306_write(oled, 0x20, COMMAND);
	ssd1306_write(oled, 0x00, COMMAND);
	// Set segment remap with column address 0 mapped to segment 0
	ssd1306_write(oled, 0xA0, COMMAND);
	ssd1306_write(oled, 0xC0, COMMAND);
	// set COM Pin hardware configuration
	ssd1306_write(oled, 0xDA, COMMAND);
	ssd1306_write(oled, 0x12, COMMAND);
	// set contrast control
	ssd1306_write(oled, 0x81, COMMAND);
	ssd1306_write(oled, 0x7F, COMMAND);
	// Set pre-charge period
	ssd1306_write(oled, 0xD9, COMMAND);
	ssd1306_write(oled, 0xF1, COMMAND);
	// Set Vcomh deselect level
	ssd1306_write(oled, 0xDB, COMMAND);
	ssd1306_write(oled, 0x20, COMMAND);
	// disable entire display on
	ssd1306_write(oled, 0xA4, COMMAND);
	// set normal display
	ssd1306_write(oled, 0xA6, COMMAND); // A6 normal a7 inverse
	// set segment re-map
	ssd1306_write(oled, 0xA0, COMMAND);
	// deactive scroll
	ssd1306_write(oled, 0x2E, COMMAND);
	// display on
	ssd1306_write(oled, 0xAF, COMMAND);
	// clear screen
	ssd1306_clear(oled);
}
static void ssd1306_clear(struct ssd1306 *oled)
{
	int i;
	ssd1306_goto_xy(oled, 0, 0);
	for (i = 0; i < OLED_WIDTH * (OLED_HEIGHT / 8 - 1); i++)
		ssd1306_write(oled, 0, DATA);
}
static void ssd1306_goto_xy(struct ssd1306 *oled, u8 x, u8 y)
{
	ssd1306_write(oled, 0x21, COMMAND);
	ssd1306_write(oled, x * FONT_X, COMMAND);
	ssd1306_write(oled, OLED_WIDTH - 1, COMMAND);
	ssd1306_write(oled, 0x22, COMMAND);
	ssd1306_write(oled, y, COMMAND);
	ssd1306_write(oled, max_Y, COMMAND);
	oled->current_X = x;
	oled->current_Y = y;
}
static void ssd1306_send_char(struct ssd1306 *oled, u8 data)
{
	if (oled->current_X == max_X - 1)
		ssd1306_go_to_next_line(oled);
	ssd1306_burst_write(oled, ssd1306_font[data - 32], FONT_X, DATA);
	oled->current_X++;
}
static void ssd1306_send_char_inv(struct ssd1306 *oled, u8 data)
{
	u8 i;
	u8 buff[FONT_X];
	if (oled->current_X == max_X - 1)
		ssd1306_go_to_next_line(oled);
	for (i = 0; i < FONT_X; i++)
		buff[i] = ~ssd1306_font[data - 32][i];
	ssd1306_burst_write(oled, buff, FONT_X, DATA);
	oled->current_X++;
}
static void ssd1306_send_string(struct ssd1306 *oled, u8 *str, color_t color)
{
	while (*str)
	{
		if (color == COLOR_WHITE)
			ssd1306_send_char(oled, *str++);
		else
			ssd1306_send_char_inv(oled, *str++);
	}
}
static void ssd1306_go_to_next_line(struct ssd1306 *oled)
{
	oled->current_Y = (oled->current_Y == max_Y - 1) ? 0 : (oled->current_Y + 1);
	ssd1306_goto_xy(oled, 0, oled->current_Y);
}
static int embedded_to_buffer(struct ssd1306 *oled, u8 *data, int start, int length)
{
	oled->current_index = FONT_X * start;
	if (length > frame_size)
		return -1;
	for (int i = 0; i < length; i++)
	{
		memcpy(&oled->frame_buffer[oled->current_index], ssd1306_font[*data++ - 32], 6);
		oled->current_index += 6;
	}
	return 0;
}
static void ssd1306_sync(struct ssd1306 *oled)
{
	int i;
	int index = 0;
	ssd1306_goto_xy(oled, 0, 0);
	for (i = 0; i < max_Y; i++)
	{
		ssd1306_burst_write(oled, &oled->frame_buffer[index], max_X * FONT_X, DATA);
		ssd1306_go_to_next_line(oled);
		index += max_X * FONT_X;
	}
}
static void animation(struct work_struct *work)
{
	struct ssd1306 *oled = container_of(work, struct ssd1306, workqueue);
	if (oled)
	{
		if (oled->gameover == FALSE)
		{
			snake_game_draw(oled);
			snake_game_logic(oled);
		}
		else
		{
			oled->button = PAUSE;
			ssd1306_goto_xy(oled, 5, 4);
			ssd1306_send_string(oled, "Game Over!", COLOR_WHITE);
		}
	}
	mod_timer(&oled->my_timer, jiffies + HZ / speed);
}
static void tmHandler(struct timer_list *tm)
{
	struct ssd1306 *oled = container_of(tm, struct ssd1306, my_timer);
	if (oled)
		schedule_work(&oled->workqueue);
}
static u8 create_random_number(u8 MAX)
{
	u8 random_number;
	get_random_bytes(&random_number, sizeof(random_number));
	return random_number % MAX;
}
/* Snake Game */
irqreturn_t buttonHandler(int irq, void *dev_id)
{
	struct ssd1306 *oled = (struct ssd1306 *)dev_id;
	if (irq == oled->button_irq[0] && (oled->mySnake[0].direction != DOWN))
		oled->button = UP;
	else if (irq == oled->button_irq[1] && (oled->mySnake[0].direction != UP))
		oled->button = DOWN;
	else if (irq == oled->button_irq[2] && (oled->mySnake[0].direction != RIGHT))
		oled->button = LEFT;
	else if (irq == oled->button_irq[3] && (oled->mySnake[0].direction != LEFT))
		oled->button = RIGHT;
	return IRQ_HANDLED;
}
static void snake_game_setup(struct ssd1306 *oled)
{
	oled->button = PAUSE;
	oled->gameover = FALSE;
	oled->mySnake[0].x = max_X / 2;
	oled->mySnake[0].y = max_Y / 2;
	while (!oled->myFood.x)
		oled->myFood.x = create_random_number(max_X - 2);
	while (oled->myFood.y < 2)
		oled->myFood.y = create_random_number(max_Y - 2);
	snake_update_score(oled, 0);
}
static void snake_update_score(struct ssd1306 *oled, u32 score)
{
	u8 scoreBuffer[21];
	memset(scoreBuffer, 0, sizeof(scoreBuffer));
	sprintf(scoreBuffer, "Score: %d", score);
	embedded_to_buffer(oled, scoreBuffer, 0, strlen(scoreBuffer));
}
static void snake_game_draw(struct ssd1306 *oled)
{
	int i, j, k;
	bool check = FALSE;
	oled->current_index = 6 * max_X;
	memset(oled->frame_buffer, 0, frame_size);
	for (i = 1; i < max_Y; i++)
	{
		for (j = 0; j < max_X; j++)
		{
			if (i == 1 || i == max_Y - 1 || j == 0 || j == max_X - 1)
				memcpy(&oled->frame_buffer[oled->current_index], ssd1306_font['+' - 32], 6);
			else
			{
				if (i == oled->mySnake[0].y && j == oled->mySnake[0].x)
				{
					switch (oled->mySnake[0].direction)
					{
					case UP:
						memcpy(&oled->frame_buffer[oled->current_index], ssd1306_font['^' - 32], 6);
						break;
					case DOWN:
						memcpy(&oled->frame_buffer[oled->current_index], ssd1306_font['v' - 32], 6);
						break;
					case LEFT:
						memcpy(&oled->frame_buffer[oled->current_index], ssd1306_font['<' - 32], 6);
						break;
					case RIGHT:
						memcpy(&oled->frame_buffer[oled->current_index], ssd1306_font['>' - 32], 6);
						break;
					default:
						memcpy(&oled->frame_buffer[oled->current_index], ssd1306_font['?' - 32], 6);
						break;
					}
				}
				else if (i == oled->myFood.y && j == oled->myFood.x)
					memcpy(&oled->frame_buffer[oled->current_index], ssd1306_font['*' - 32], 6);
				else
				{
					for (k = 1; k < oled->current_length; k++)
					{
						if (i == oled->mySnake[k].y && j == oled->mySnake[k].x)
						{
							memcpy(&oled->frame_buffer[oled->current_index], ssd1306_font['o' - 32], 6);
							check = TRUE;
							break;
						}
					}
					if (!check)
						memcpy(&oled->frame_buffer[oled->current_index], ssd1306_font[' ' - 32], 6);
				}
			}
			oled->current_index += 6;
		}
	}
	snake_update_score(oled, oled->score);
	ssd1306_sync(oled);
}
static void move(struct snake *snk)
{
	switch (snk->direction)
	{
	case UP:
		snk->y--;
		break;
	case DOWN:
		snk->y++;
		break;
	case LEFT:
		snk->x--;
		break;
	case RIGHT:
		snk->x++;
		break;
	default:
		break;
	}
}
static void snake_game_logic(struct ssd1306 *oled)
{
	u8 i, j;
	bool die = FALSE;
	switch (oled->button)
	{
	case UP:
		oled->mySnake[0].direction = UP;
		oled->mySnake[0].y--;
		break;
	case DOWN:
		oled->mySnake[0].direction = DOWN;
		oled->mySnake[0].y++;
		break;
	case LEFT:
		oled->mySnake[0].direction = LEFT;
		oled->mySnake[0].x--;
		break;
	case RIGHT:
		oled->mySnake[0].direction = RIGHT;
		oled->mySnake[0].x++;
		break;
	default:
		break;
	}
	for (i = 1; i < oled->current_length; i++)
	{
		switch (oled->mySnake[i - 1].direction)
		{
		case UP:
			if (oled->mySnake[i].x == oled->mySnake[i - 1].x)
				oled->mySnake[i].direction = UP;
			move(&oled->mySnake[i]);
			break;
		case DOWN:
			if (oled->mySnake[i].x == oled->mySnake[i - 1].x)
				oled->mySnake[i].direction = DOWN;
			move(&oled->mySnake[i]);
			break;
		case LEFT:
			if (oled->mySnake[i].y == oled->mySnake[i - 1].y)
				oled->mySnake[i].direction = LEFT;
			move(&oled->mySnake[i]);
			break;
		case RIGHT:
			if (oled->mySnake[i].y == oled->mySnake[i - 1].y)
				oled->mySnake[i].direction = RIGHT;
			move(&oled->mySnake[i]);
			break;
		default:
			break;
		}
	}
	for (i = 0; i < oled->current_length; i++) // collision check
	{
		for (j = i + 1; j < oled->current_length; j++)
		{
			if (oled->mySnake[i].x == oled->mySnake[j].x && oled->mySnake[i].y == oled->mySnake[j].y && oled->current_length > 1)
			{
				oled->gameover = TRUE;
				die = TRUE;
				break;
			}
		}
		if (die)
			break;
	}
	if (oled->mySnake[0].x <= 0 || oled->mySnake[0].x >= max_X || oled->mySnake[0].y < 2 || oled->mySnake[0].y >= max_Y) // wall collision
		oled->gameover = TRUE;
	if (oled->mySnake[0].x == oled->myFood.x && oled->mySnake[0].y == oled->myFood.y) // ate food
	{
		add_new_element(oled);
		oled->myFood.x = 0;
		oled->myFood.y = 0;
		while (!oled->myFood.x)
			oled->myFood.x = create_random_number(max_X - 2);
		while (oled->myFood.y < 2)
			oled->myFood.y = create_random_number(max_Y - 2);
		oled->score += 10;
	}
}
static int add_new_element(struct ssd1306 *oled)
{
	u8 new_index = oled->current_length;
	oled->current_length += 1;
	struct snake *newSnake = kzalloc(oled->current_length * sizeof(struct snake), GFP_KERNEL);
	if (!newSnake)
		return -1;
	memcpy(newSnake, oled->mySnake, (oled->current_length - 1) * sizeof(struct snake));
	kfree(oled->mySnake);
	oled->mySnake = newSnake;
	switch (oled->mySnake[new_index - 1].direction)
	{
	case UP:
		oled->mySnake[new_index].x = oled->mySnake[new_index - 1].x;
		oled->mySnake[new_index].y = oled->mySnake[new_index - 1].y + 1;
		oled->mySnake[new_index].direction = oled->mySnake[new_index - 1].direction;
		break;
	case DOWN:
		oled->mySnake[new_index].x = oled->mySnake[new_index - 1].x;
		oled->mySnake[new_index].y = oled->mySnake[new_index - 1].y - 1;
		oled->mySnake[new_index].direction = oled->mySnake[new_index - 1].direction;
		break;
	case LEFT:
		oled->mySnake[new_index].x = oled->mySnake[new_index - 1].x + 1;
		oled->mySnake[new_index].y = oled->mySnake[new_index - 1].y;
		oled->mySnake[new_index].direction = oled->mySnake[new_index - 1].direction;
		break;
	case RIGHT:
		oled->mySnake[new_index].x = oled->mySnake[new_index - 1].x - 1;
		oled->mySnake[new_index].y = oled->mySnake[new_index - 1].y;
		oled->mySnake[new_index].direction = oled->mySnake[new_index - 1].direction;
		break;
	default:
		break;
	}
	return 0;
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("DinhNam <20021163@vnu.edu.vn>");
MODULE_DESCRIPTION("Snake Game with oled ssd1306");
MODULE_VERSION("1.0");
